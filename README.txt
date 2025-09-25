# Book Tracer (Console)

Track your books in a tiny SQLite database, look up titles/authors by ISBN using **Open Library** (always works) and **Google Books** (optional, with API key), and estimate days left to finish based on your **daily reading rate**.

## Features
- Add books manually or by ISBN-10/13 (online lookup)
- Update current page; mark status (To-Read / Reading / Finished)
- Search & filtered lists
- Daily reading rate → ETA (days left) per book
- Export/Import CSV
- Works offline (Open Library fallback & local DB)

## Build & Run (Windows, MSYS2 UCRT64 + VS Code)
1. Install **MSYS2** and use the **UCRT64** environment.
2. Install deps in UCRT64:
   ```bash
   pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-sqlite3 mingw-w64-ucrt-x86_64-curl
