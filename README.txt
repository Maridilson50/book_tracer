@'
# Book Tracer (console)

Track books in SQLite. Optional ISBN lookup via Open Library and Google Books.

## Build & Run
- VS Code: **Ctrl+Shift+B** → “Build & Run (MSYS2 UCRT64)”
- Or press **F5** to debug.

## Google Books key (optional)
`setx GOOGLE_BOOKS_API_KEY "YOUR_KEY"`  
Restart VS Code after changing env vars.
'@ | Out-File -Encoding utf8 README.md
