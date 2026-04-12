#ifndef IRON_CLI_WEB_SHELL_TEMPLATE_H
#define IRON_CLI_WEB_SHELL_TEMPLATE_H

/*
 * Default Iron web shell HTML, embedded as a C string literal.
 *
 * Consumed by src/cli/build_web.c (Plan 09-02) which materializes this
 * string to a temp file and passes it to emcc via --shell-file. Users who
 * want a custom shell set [web].shell in iron.toml; the custom path is
 * validated to contain {{{ SCRIPT }}} before being passed to emcc.
 *
 * Derived from src/vendor/raylib/minshell.html with four patches:
 *   - COOP/COEP preflight (red error banner when !crossOriginIsolated)  [WEB-SHELL-04]
 *   - <canvas id="canvas"> element preserved from minshell              [WEB-SHELL-03]
 *   - Audio unlock (keydown/pointerdown -> Module._audio_resume())      [WEB-SHELL-02, WEB-AUDIO-*]
 *   - webglcontextlost handler (visible reload prompt)                  [WEB-SHELL-07]
 *
 * The emcc substitution slot {{{ SCRIPT }}} is preserved verbatim.      [WEB-SHELL-05]
 */
static const char IRON_WEB_DEFAULT_SHELL[] =
    "<!doctype html>\n"
    "<html lang=\"EN-us\">\n"
    "  <head>\n"
    "    <meta charset=\"utf-8\">\n"
    "    <meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">\n"
    "    <title>Iron Web Build</title>\n"
    "    <meta name=\"viewport\" content=\"width=device-width\">\n"
    "    <style>\n"
    "      body { margin: 0px; overflow: hidden; background-color: black; }\n"
    "      canvas.emscripten { border: 0px none; background-color: black; }\n"
    "    </style>\n"
    "    <script>\n"
    "      if (!self.crossOriginIsolated) {\n"
    "        document.body.innerHTML = '<div style=\"background:red;color:white;padding:20px;"
    "font-family:monospace;font-size:16px;\">ERROR: This Iron web build requires COOP+COEP "
    "headers for SharedArrayBuffer. Server must send: Cross-Origin-Opener-Policy: same-origin "
    "and Cross-Origin-Embedder-Policy: require-corp. See Iron docs for deployment.</div>';\n"
    "        throw new Error('COOP/COEP headers missing');\n"
    "      }\n"
    "    </script>\n"
    "  </head>\n"
    "  <body>\n"
    "    <canvas class=emscripten id=canvas oncontextmenu=event.preventDefault() tabindex=-1></canvas>\n"
    "    <p id=\"output\"></p>\n"
    "    <script>\n"
    "      var Module = {\n"
    "        print: (function() {\n"
    "          var element = document.getElementById('output');\n"
    "          if (element) element.value = '';\n"
    "          return function(text) {\n"
    "            if (arguments.length > 1) text = Array.prototype.slice.call(arguments).join(' ');\n"
    "            console.log(text);\n"
    "            if (element) {\n"
    "              element.value += text + \"\\n\";\n"
    "              element.scrollTop = element.scrollHeight;\n"
    "            }\n"
    "          };\n"
    "        })(),\n"
    "        canvas: (function() {\n"
    "          var canvas = document.getElementById('canvas');\n"
    "          return canvas;\n"
    "        })()\n"
    "      };\n"
    "    </script>\n"
    "    <script>\n"
    "      function unlockAudio() {\n"
    "        if (typeof Module !== 'undefined' && Module._audio_resume) Module._audio_resume();\n"
    "        document.removeEventListener('keydown', unlockAudio);\n"
    "        document.removeEventListener('pointerdown', unlockAudio);\n"
    "      }\n"
    "      document.addEventListener('keydown', unlockAudio, { once: false });\n"
    "      document.addEventListener('pointerdown', unlockAudio, { once: false });\n"
    "    </script>\n"
    "    <script>\n"
    "      document.getElementById('canvas').addEventListener('webglcontextlost', function(e) {\n"
    "        e.preventDefault();\n"
    "        document.body.innerHTML += '<div style=\"background:orange;color:black;padding:20px;"
    "font-family:monospace;\">WebGL context lost. Please reload the page.</div>';\n"
    "      }, false);\n"
    "    </script>\n"
    "    {{{ SCRIPT }}}\n"
    "  </body>\n"
    "</html>\n";

#endif /* IRON_CLI_WEB_SHELL_TEMPLATE_H */
