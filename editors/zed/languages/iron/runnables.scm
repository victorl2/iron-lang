; Run buttons in the gutter: a test block runs on its own (tag iron-test,
; with its name in $ZED_CUSTOM_test_name), `func main` runs the file
; (tag iron-main). The tasks are in tasks.json; the Iron debug locator
; turns either into a debug session.

(
  (test_declaration
    "test" @run
    name: (string_literal) @test_name)
  (#set! tag iron-test)
)

(
  (func_declaration
    "func" @run
    name: (identifier) @_name)
  (#eq? @_name "main")
  (#set! tag iron-main)
)
