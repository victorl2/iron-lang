// Go baseline for the apps/inventory benchmarks: the endpoints the
// benchmark drives, written the idiomatic Go way (net/http, database/sql
// with go-sql-driver/mysql and prepared statements, encoding/json), against
// the same schema, the same MySQL server and the same exchange rate stub.
//
//	INVENTORY_PORT, INVENTORY_DSN (go-sql-driver DSN), INVENTORY_RATES_URL,
//	INVENTORY_DB_POOL_MAX
//
// The schema is the one apps/inventory migrates; run the Iron service once
// against the database first.
package main

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"log"
	"math"
	"net/http"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/go-sql-driver/mysql"
)

type Item struct {
	ID         int64   `json:"id"`
	SKU        string  `json:"sku"`
	Name       string  `json:"name"`
	Category   string  `json:"category"`
	PriceCents int64   `json:"price_cents"`
	Quantity   int64   `json:"quantity"`
	CreatedAt  string  `json:"created_at"`
	UpdatedAt  string  `json:"updated_at"`
	Price      *Priced `json:"price,omitempty"`
}

type Priced struct {
	Currency    string  `json:"currency"`
	AmountCents int64   `json:"amount_cents"`
	Rate        float64 `json:"rate"`
	Stale       bool    `json:"stale"`
	AsOf        string  `json:"as_of"`
}

type rateCache struct {
	mu      sync.RWMutex
	rates   map[string]float64
	asOf    string
	fetched time.Time
}

type app struct {
	db       *sql.DB
	getItem  *sql.Stmt
	selQty   string
	rates    rateCache
	ratesURL string
	client   *http.Client
	ttl      time.Duration
}

const itemCols = "id, sku, name, category, price_cents, quantity, created_at, updated_at"

func ts(t time.Time) string { return t.UTC().Format("2006-01-02T15:04:05.000000Z") }

func scanItem(s interface{ Scan(...any) error }) (Item, error) {
	var it Item
	var c, u time.Time
	err := s.Scan(&it.ID, &it.SKU, &it.Name, &it.Category, &it.PriceCents, &it.Quantity, &c, &u)
	it.CreatedAt, it.UpdatedAt = ts(c), ts(u)
	return it, err
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(v)
}

func errJSON(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]string{"error": msg})
}

func (a *app) refresh() error {
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	req, _ := http.NewRequestWithContext(ctx, "GET", a.ratesURL+"/latest?from=USD", nil)
	resp, err := a.client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return errors.New(resp.Status)
	}
	var body struct {
		Base  string             `json:"base"`
		Date  string             `json:"date"`
		Rates map[string]float64 `json:"rates"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		return err
	}
	a.rates.mu.Lock()
	a.rates.rates, a.rates.asOf, a.rates.fetched = body.Rates, body.Date, time.Now()
	a.rates.mu.Unlock()
	return nil
}

func (a *app) refreshLoop() {
	for {
		a.rates.mu.RLock()
		age := time.Since(a.rates.fetched)
		a.rates.mu.RUnlock()
		if age >= a.ttl/2 {
			if err := a.refresh(); err != nil {
				log.Printf("refresh: %v", err)
			}
		}
		time.Sleep(100 * time.Millisecond)
	}
}

func (a *app) rate(code string) (float64, bool, string, bool) {
	code = strings.ToUpper(code)
	if code == "USD" {
		return 1, false, "", true
	}
	a.rates.mu.RLock()
	defer a.rates.mu.RUnlock()
	r, ok := a.rates.rates[code]
	return r, time.Since(a.rates.fetched) >= a.ttl, a.rates.asOf, ok
}

func (a *app) items(w http.ResponseWriter, r *http.Request) {
	rest := strings.TrimPrefix(r.URL.Path, "/v1/items")
	if rest == "" || rest == "/" {
		switch r.Method {
		case "GET":
			a.list(w, r)
		case "POST":
			a.create(w, r)
		default:
			errJSON(w, 405, "method not allowed")
		}
		return
	}
	parts := strings.Split(strings.TrimPrefix(rest, "/"), "/")
	id, err := strconv.ParseInt(parts[0], 10, 64)
	if err != nil || id <= 0 {
		errJSON(w, 404, "item not found")
		return
	}
	if len(parts) == 1 && r.Method == "GET" {
		a.get(w, r, id)
		return
	}
	if len(parts) == 2 && parts[1] == "stock" && r.Method == "POST" {
		a.stock(w, r, id)
		return
	}
	errJSON(w, 404, "not found")
}

func (a *app) get(w http.ResponseWriter, r *http.Request, id int64) {
	var conv *Priced
	if cur := r.URL.Query().Get("currency"); cur != "" {
		rate, stale, asOf, ok := a.rate(cur)
		if !ok {
			errJSON(w, 400, "unknown currency")
			return
		}
		conv = &Priced{Currency: strings.ToUpper(cur), Rate: rate, Stale: stale, AsOf: asOf}
	}
	it, err := scanItem(a.getItem.QueryRowContext(r.Context(), id))
	if errors.Is(err, sql.ErrNoRows) {
		errJSON(w, 404, "item not found")
		return
	} else if err != nil {
		errJSON(w, 503, "database unavailable, retry later")
		return
	}
	if conv != nil {
		conv.AmountCents = int64(math.Round(float64(it.PriceCents) * conv.Rate))
		it.Price = conv
	}
	writeJSON(w, 200, it)
}

func (a *app) list(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	limit := 50
	if s := q.Get("limit"); s != "" {
		n, err := strconv.Atoi(s)
		if err != nil || n < 1 || n > 500 {
			errJSON(w, 400, "bad limit")
			return
		}
		limit = n
	}
	after, _ := strconv.ParseInt(q.Get("after"), 10, 64)
	sqlText := "SELECT " + itemCols + " FROM items WHERE id > ?"
	args := []any{after}
	if c := q.Get("category"); c != "" {
		sqlText += " AND category = ?"
		args = append(args, c)
	}
	sqlText += " ORDER BY id LIMIT ?"
	args = append(args, limit+1)
	rows, err := a.db.QueryContext(r.Context(), sqlText, args...)
	if err != nil {
		errJSON(w, 503, "database unavailable, retry later")
		return
	}
	defer rows.Close()
	items := []Item{}
	for rows.Next() {
		it, err := scanItem(rows)
		if err != nil {
			errJSON(w, 500, "internal error")
			return
		}
		items = append(items, it)
	}
	var next *int64
	if len(items) > limit {
		items = items[:limit]
		n := items[limit-1].ID
		next = &n
	}
	writeJSON(w, 200, map[string]any{"items": items, "count": len(items), "next_after": next})
}

func (a *app) create(w http.ResponseWriter, r *http.Request) {
	var in struct {
		SKU        string `json:"sku"`
		Name       string `json:"name"`
		Category   string `json:"category"`
		PriceCents *int64 `json:"price_cents"`
		Quantity   int64  `json:"quantity"`
	}
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 65536)).Decode(&in); err != nil {
		errJSON(w, 400, "malformed JSON")
		return
	}
	if in.SKU == "" || in.Name == "" || in.Category == "" || in.PriceCents == nil {
		errJSON(w, 422, "validation failed")
		return
	}
	tx, err := a.db.BeginTx(r.Context(), nil)
	if err != nil {
		errJSON(w, 503, "database unavailable, retry later")
		return
	}
	defer tx.Rollback()
	res, err := tx.Exec("INSERT INTO items (sku, name, category, price_cents, quantity) VALUES (?, ?, ?, ?, ?)",
		in.SKU, in.Name, in.Category, *in.PriceCents, in.Quantity)
	var me *mysql.MySQLError
	if errors.As(err, &me) && me.Number == 1062 {
		errJSON(w, 409, "an item with this sku already exists")
		return
	} else if err != nil {
		errJSON(w, 503, "database unavailable, retry later")
		return
	}
	id, _ := res.LastInsertId()
	if in.Quantity != 0 {
		if _, err := tx.Exec("INSERT INTO stock_movements (item_id, delta, reason) VALUES (?, ?, 'initial')", id, in.Quantity); err != nil {
			errJSON(w, 503, "database unavailable, retry later")
			return
		}
	}
	if err := tx.Commit(); err != nil {
		errJSON(w, 503, "database unavailable, retry later")
		return
	}
	it, err := scanItem(a.getItem.QueryRowContext(r.Context(), id))
	if err != nil {
		errJSON(w, 500, "internal error")
		return
	}
	writeJSON(w, 201, it)
}

func (a *app) stock(w http.ResponseWriter, r *http.Request, id int64) {
	var in struct {
		Delta  int64  `json:"delta"`
		Reason string `json:"reason"`
	}
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 65536)).Decode(&in); err != nil || in.Delta == 0 {
		errJSON(w, 422, "validation failed")
		return
	}
	if in.Reason == "" {
		in.Reason = "adjustment"
	}
	for attempt := 0; ; attempt++ {
		q, mid, status, err := a.stockOnce(r.Context(), id, in.Delta, in.Reason)
		var me *mysql.MySQLError
		if errors.As(err, &me) && (me.Number == 1213 || me.Number == 1205) && attempt < 2 {
			continue
		}
		switch {
		case status == 200:
			writeJSON(w, 200, map[string]int64{"id": id, "quantity": q, "movement_id": mid})
		case status == 404:
			errJSON(w, 404, "item not found")
		case status == 409:
			writeJSON(w, 409, map[string]any{"error": "insufficient stock", "quantity": q, "requested": in.Delta})
		default:
			errJSON(w, 503, "database unavailable, retry later")
		}
		return
	}
}

func (a *app) stockOnce(ctx context.Context, id, delta int64, reason string) (int64, int64, int, error) {
	tx, err := a.db.BeginTx(ctx, nil)
	if err != nil {
		return 0, 0, 503, err
	}
	defer tx.Rollback()
	var q int64
	err = tx.QueryRow("SELECT quantity FROM items WHERE id = ? FOR UPDATE", id).Scan(&q)
	if errors.Is(err, sql.ErrNoRows) {
		return 0, 0, 404, nil
	} else if err != nil {
		return 0, 0, 503, err
	}
	if q+delta < 0 {
		return q, 0, 409, nil
	}
	if _, err := tx.Exec("UPDATE items SET quantity = ? WHERE id = ?", q+delta, id); err != nil {
		return 0, 0, 503, err
	}
	res, err := tx.Exec("INSERT INTO stock_movements (item_id, delta, reason) VALUES (?, ?, ?)", id, delta, reason)
	if err != nil {
		return 0, 0, 503, err
	}
	mid, _ := res.LastInsertId()
	if err := tx.Commit(); err != nil {
		return 0, 0, 503, err
	}
	return q + delta, mid, 200, nil
}

func (a *app) rateHandler(w http.ResponseWriter, r *http.Request) {
	code := strings.TrimPrefix(r.URL.Path, "/v1/rates/")
	rate, stale, asOf, ok := a.rate(code)
	if !ok {
		errJSON(w, 400, "unknown currency")
		return
	}
	a.rates.mu.RLock()
	age := time.Since(a.rates.fetched).Milliseconds()
	a.rates.mu.RUnlock()
	writeJSON(w, 200, map[string]any{"base": "USD", "currency": strings.ToUpper(code), "rate": rate,
		"stale": stale, "as_of": asOf, "age_ms": age})
}

func main() {
	dsn := os.Getenv("INVENTORY_DSN")
	db, err := sql.Open("mysql", dsn)
	if err != nil {
		log.Fatal(err)
	}
	poolMax, _ := strconv.Atoi(os.Getenv("INVENTORY_DB_POOL_MAX"))
	if poolMax <= 0 {
		poolMax = 32
	}
	db.SetMaxOpenConns(poolMax)
	db.SetMaxIdleConns(poolMax)
	db.SetConnMaxLifetime(30 * time.Minute)
	getItem, err := db.Prepare("SELECT " + itemCols + " FROM items WHERE id = ?")
	if err != nil {
		log.Fatal(err)
	}
	a := &app{db: db, getItem: getItem, ratesURL: os.Getenv("INVENTORY_RATES_URL"),
		client: &http.Client{Timeout: time.Second}, ttl: 60 * time.Second}
	go a.refreshLoop()
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, map[string]string{"status": "ok"})
	})
	mux.HandleFunc("/v1/items", a.items)
	mux.HandleFunc("/v1/items/", a.items)
	mux.HandleFunc("/v1/rates/", a.rateHandler)
	port := os.Getenv("INVENTORY_PORT")
	if port == "" {
		port = "8080"
	}
	log.Printf("listening on %s", port)
	srv := &http.Server{Addr: "127.0.0.1:" + port, Handler: mux, ReadTimeout: 5 * time.Second, IdleTimeout: 30 * time.Second}
	log.Fatal(srv.ListenAndServe())
}
