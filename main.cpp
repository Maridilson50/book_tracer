// main.cpp — Console Book Tracer (SQLite + ISBN lookup)
// Dependencies: sqlite3, libcurl, nlohmann/json (header-only)

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <sqlite3.h>
#include <curl/curl.h>
// nlohmann/json is header-only; ensure include path is set via vcpkg or your environment.
#include <nlohmann/json.hpp>

struct Book {
    int         id = 0;
    std::string title;
    std::string author;
    int         totalPages = 0;
    int         currentPage = 0;
    int         status = 0;      // 0 To-Read, 1 Reading, 2 Finished
    std::string isbn;            // optional
};

static bool g_useGoogleBooks = true;  // can be turned off if check fails

enum class Status { ToRead=0, Reading=1, Finished=2 };

static inline std::string statusToStr(Status s) {
    switch (s) {
        case Status::ToRead:  return "To-Read";
        case Status::Reading: return "Reading";
        case Status::Finished:return "Finished";
    }
    return "To-Read";
}
static inline std::optional<Status> strToStatus(const std::string& s) {
    std::string t = s;
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c){ return std::tolower(c); });
    if (t=="to-read" || t=="toread" || t=="todo" || t=="0") return Status::ToRead;
    if (t=="reading" || t=="1") return Status::Reading;
    if (t=="finished"|| t=="done" || t=="2") return Status::Finished;
    return std::nullopt;
}

static inline double percentComplete(const Book& b) {
    if (b.totalPages <= 0) return 0.0;
    return 100.0 * static_cast<double>(b.currentPage) / static_cast<double>(b.totalPages);
}
static inline std::optional<int> daysToFinish(const Book& b, int dailyRate) {
    if (dailyRate <= 0 || b.totalPages <= b.currentPage) return std::nullopt;
    int remaining = b.totalPages - b.currentPage;
    // ceil division:
    return (remaining + dailyRate - 1) / dailyRate;
}

// ----------------------------- Small IO helpers -----------------------------
static int askInt(const std::string& prompt, int lo, int hi) {
    while (true) {
        std::cout << prompt << " " << std::flush;
        std::string s; if (!std::getline(std::cin, s)) return lo;
        try {
            int v = std::stoi(s);
            if (v<lo || v>hi) { std::cout << "Enter a number in ["<<lo<<","<<hi<<"].\n"; continue; }
            return v;
        } catch (...) { std::cout << "Invalid number. Try again.\n"; }
    }
}
static std::string askLine(const std::string& prompt, bool allowEmpty=false) {
    while (true) {
        std::cout << prompt << " " << std::flush;
        std::string s; std::getline(std::cin, s);
        if (!allowEmpty && s.empty()) { std::cout << "Please enter something.\n"; continue; }
        return s;
    }
}

// ----------------------------- ISBN utilities ------------------------------
static std::string onlyDigitsX(const std::string& s) {
    std::string t; t.reserve(s.size());
    for (char c: s) if (std::isdigit((unsigned char)c) || c=='X' || c=='x') t.push_back(c=='x'?'X':c);
    return t;
}
static bool isIsbn10(const std::string& s) { return s.size()==10; }
static bool isIsbn13(const std::string& s) { return s.size()==13; }

// convert ISBN-10 to ISBN-13 (prefix 978 and recompute)
static std::string isbn10to13(const std::string& s10) {
    std::string core = "978" + s10.substr(0, 9);
    int sum = 0;
    for (int i=0;i<(int)core.size();++i) {
        int d = core[i]-'0';
        sum += (i%2==0) ? d : 3*d;
    }
    int cd = (10 - (sum % 10)) % 10;
    return core + std::to_string(cd);
}
static std::string normalizeIsbn(const std::string& in) {
    std::string s = onlyDigitsX(in);
    if (isIsbn13(s)) return s;
    if (isIsbn10(s)) return isbn10to13(s);
    return ""; // invalid
}

// ----------------------------- HTTP via curl -------------------------------
static size_t curlWrite(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = reinterpret_cast<std::string*>(userdata);
    out->append(reinterpret_cast<const char*>(ptr), size*nmemb);
    return size*nmemb;
}
static std::optional<std::string> httpGet(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) return std::nullopt;
    std::string buf;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "BookTracer/1.0");
    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK || code < 200 || code >= 300) return std::nullopt;
    return buf;
}

// Quick console status line
static void printStep(const char* what, bool ok) {
    std::cout << std::left << std::setw(36) << what
              << (ok ? "Passed!\n" : "FAILED\n");
}

// Ultra-light internet probe (returns true on any 2xx)
static bool internetOk() {
    // 204 No Content on success; our httpGet treats 2xx as OK
    return static_cast<bool>(httpGet("https://www.google.com/generate_204"));
}

// Quick Open Library ping (home page is fine; returns 200)
static bool openLibraryOk() {
    return static_cast<bool>(httpGet("https://openlibrary.org/"));
}

// Key presence only (no network). For full check we’ll still call googleBooksReady().
static bool googleKeyPresent() {
    const char* k = std::getenv("GOOGLE_BOOKS_API_KEY");
    return k && *k;
}

// Quick Google Books API readiness check.
// Returns true if key exists AND a tiny request succeeds (HTTP 2xx and no "error").
static bool googleBooksReady() {
    const char* key = std::getenv("GOOGLE_BOOKS_API_KEY");
    if (!key || !*key) return false;  // no key

    // Tiny request; OK even if totalItems==0
    std::string url =
        "https://www.googleapis.com/books/v1/volumes?q=isbn:0000000000000&maxResults=1&fields=totalItems&key=";
    url += key;

    if (auto body = httpGet(url)) {
        try {
            auto j = nlohmann::json::parse(*body);
            if (j.contains("error")) return false;  // key invalid/blocked
            return true;
        } catch (...) {
            return false; // not JSON -> treat as failure
        }
    }
    return false; // network/HTTP error
}


struct LookupResult { std::string title; std::string author; };
static std::optional<LookupResult> lookupIsbn(const std::string& rawIsbn) {
    std::string isbn13 = normalizeIsbn(rawIsbn);
    if (isbn13.empty()) return std::nullopt;

    // 1) Open Library (no key)
    {
        std::string url = "https://openlibrary.org/isbn/" + isbn13 + ".json";
        if (auto body = httpGet(url)) {
            try {
                auto j = nlohmann::json::parse(*body);
                LookupResult r;
                if (j.contains("title")) r.title = j["title"].get<std::string>();
                // author handling: Open Library authors usually need a 2nd request;
                // try by_statement if present, else leave blank and let Google fill.
                if (j.contains("by_statement")) r.author = j["by_statement"].get<std::string>();
                if (!r.title.empty()) {
                    return r; // may have empty author, that's fine for now
                }
            } catch (...) {}
        }
    }

    // 2) Google Books (needs key for higher reliability, but can work without)
    if (g_useGoogleBooks) {
        const char* key = std::getenv("GOOGLE_BOOKS_API_KEY");
        std::ostringstream oss;
        oss << "https://www.googleapis.com/books/v1/volumes?q=isbn:" << isbn13;
        if (key && *key) oss << "&key=" << key;

        if (auto body = httpGet(oss.str())) {
            try {
                auto j = nlohmann::json::parse(*body);
                if (j.contains("items") && j["items"].is_array() && !j["items"].empty()) {
                    auto vi = j["items"][0]["volumeInfo"];
                    LookupResult r;
                    if (vi.contains("title")) r.title = vi["title"].get<std::string>();
                    if (vi.contains("authors") && vi["authors"].is_array() && !vi["authors"].empty())
                        r.author = vi["authors"][0].get<std::string>();
                    if (!r.title.empty() || !r.author.empty()) return r;
                }
            } catch (...) {}
        }
    }

    return std::nullopt;
}

// ----------------------------- SQLite storage ------------------------------
class SqliteStorage {
public:
    explicit SqliteStorage(const std::string& dbpath) {
        if (sqlite3_open(dbpath.c_str(), &db_) != SQLITE_OK) {
            std::cerr << "SQLite open failed: " << sqlite3_errmsg(db_) << "\n";
            db_ = nullptr;
        } else {
            ensureSchema();
        }
    }
    ~SqliteStorage() {
        if (db_) sqlite3_close(db_);
    }
    bool ok() const { return db_ != nullptr; }

    void ensureSchema() {
        const char* sql =
            "CREATE TABLE IF NOT EXISTS books ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  title TEXT NOT NULL,"
            "  author TEXT,"
            "  total_pages INTEGER NOT NULL,"
            "  current_page INTEGER NOT NULL,"
            "  status INTEGER NOT NULL,"
            "  isbn TEXT"
            ");";
        exec(sql);

        // NEW: key-value store for app settings
        exec("CREATE TABLE IF NOT EXISTS settings ("
            "  key TEXT PRIMARY KEY,"
            "  value TEXT NOT NULL"
            ");");

        // Helpful index for LIKE searches on title/author
        exec("CREATE INDEX IF NOT EXISTS idx_books_title ON books(title);");
        exec("CREATE INDEX IF NOT EXISTS idx_books_author ON books(author);");
        exec("CREATE INDEX IF NOT EXISTS idx_books_status ON books(status);");
    }

    int add(const Book& b) {
        const char* sql =
            "INSERT INTO books(title,author,total_pages,current_page,status,isbn)"
            "VALUES(?,?,?,?,?,?);";
        sqlite3_stmt* st=nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return -1;
        sqlite3_bind_text(st, 1, b.title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, b.author.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (st, 3, b.totalPages);
        sqlite3_bind_int (st, 4, b.currentPage);
        sqlite3_bind_int (st, 5, b.status);
        sqlite3_bind_text(st, 6, b.isbn.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return -1;
        return static_cast<int>(sqlite3_last_insert_rowid(db_));
    }

    bool updateProgress(int id, int currentPage, int status) {
        const char* sql = "UPDATE books SET current_page=?, status=? WHERE id=?;";
        sqlite3_stmt* st=nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_int(st, 1, currentPage);
        sqlite3_bind_int(st, 2, status);
        sqlite3_bind_int(st, 3, id);
        int rc = sqlite3_step(st); sqlite3_finalize(st);
        return rc == SQLITE_DONE;
    }

    bool updateStatus(int id, int status) {
        const char* sql = "UPDATE books SET status=?, current_page=CASE WHEN ?=2 THEN total_pages ELSE current_page END WHERE id=?;";
        sqlite3_stmt* st=nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_int(st, 1, status);
        sqlite3_bind_int(st, 2, status);
        sqlite3_bind_int(st, 3, id);
        int rc = sqlite3_step(st); sqlite3_finalize(st);
        return rc == SQLITE_DONE;
    }

    bool remove(int id) {
        const char* sql = "DELETE FROM books WHERE id=?;";
        sqlite3_stmt* st=nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_int(st, 1, id);
        int rc = sqlite3_step(st); sqlite3_finalize(st);
        return rc == SQLITE_DONE;
    }

    std::optional<Book> get(int id) {
        const char* sql = "SELECT id,title,author,total_pages,current_page,status,isbn FROM books WHERE id=?;";
        sqlite3_stmt* st=nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return std::nullopt;
        sqlite3_bind_int(st, 1, id);
        Book b;
        if (sqlite3_step(st) == SQLITE_ROW) {
            b.id          = sqlite3_column_int(st,0);
            b.title       = reinterpret_cast<const char*>(sqlite3_column_text(st,1));
            b.author      = reinterpret_cast<const char*>(sqlite3_column_text(st,2));
            b.totalPages  = sqlite3_column_int(st,3);
            b.currentPage = sqlite3_column_int(st,4);
            b.status      = sqlite3_column_int(st,5);
            const unsigned char* is = sqlite3_column_text(st,6);
            b.isbn        = is ? reinterpret_cast<const char*>(is) : "";
            sqlite3_finalize(st);
            return b;
        }
        sqlite3_finalize(st);
        return std::nullopt;
    }

    std::vector<Book> list(std::optional<int> statusFilter = std::nullopt) {
        std::vector<Book> out;
        std::string sql = "SELECT id,title,author,total_pages,current_page,status,isbn FROM books";
        if (statusFilter) sql += " WHERE status=?";
        sql += " ORDER BY id ASC;";
        sqlite3_stmt* st=nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
        if (statusFilter) sqlite3_bind_int(st, 1, *statusFilter);
        while (sqlite3_step(st) == SQLITE_ROW) {
            Book b;
            b.id          = sqlite3_column_int(st,0);
            b.title       = reinterpret_cast<const char*>(sqlite3_column_text(st,1));
            b.author      = reinterpret_cast<const char*>(sqlite3_column_text(st,2));
            b.totalPages  = sqlite3_column_int(st,3);
            b.currentPage = sqlite3_column_int(st,4);
            b.status      = sqlite3_column_int(st,5);
            const unsigned char* is = sqlite3_column_text(st,6);
            b.isbn        = is ? reinterpret_cast<const char*>(is) : "";
            out.push_back(std::move(b));
        }
        sqlite3_finalize(st);
        return out;
    }

    std::vector<Book> search(const std::string& q) {
        std::vector<Book> out;
        const char* sql =
            "SELECT id,title,author,total_pages,current_page,status,isbn "
            "FROM books WHERE lower(title) LIKE ? OR lower(author) LIKE ? ORDER BY id ASC;";
        sqlite3_stmt* st=nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
        std::string pat = "%" + q + "%";
        std::string patLower = pat;
        std::transform(patLower.begin(), patLower.end(), patLower.begin(), [](unsigned char c){return std::tolower(c);});
        sqlite3_bind_text(st, 1, patLower.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, patLower.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            Book b;
            b.id          = sqlite3_column_int(st,0);
            b.title       = reinterpret_cast<const char*>(sqlite3_column_text(st,1));
            b.author      = reinterpret_cast<const char*>(sqlite3_column_text(st,2));
            b.totalPages  = sqlite3_column_int(st,3);
            b.currentPage = sqlite3_column_int(st,4);
            b.status      = sqlite3_column_int(st,5);
            const unsigned char* is = sqlite3_column_text(st,6);
            b.isbn        = is ? reinterpret_cast<const char*>(is) : "";
            out.push_back(std::move(b));
        }
        sqlite3_finalize(st);
        return out;
    }

    public:
    // get daily rate (pages/day); 0 if unset
    int getDailyRate() {
        const char* sql = "SELECT value FROM settings WHERE key='daily_rate';";
        sqlite3_stmt* st=nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
        int rate = 0;
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* v = sqlite3_column_text(st, 0);
            if (v) { try { rate = std::stoi(reinterpret_cast<const char*>(v)); } catch (...) {} }
        }
        sqlite3_finalize(st);
        return std::max(0, rate);
    }

    bool setDailyRate(int rate) {
        const char* sql = "INSERT INTO settings(key,value) VALUES('daily_rate',?) "
                          "ON CONFLICT(key) DO UPDATE SET value=excluded.value;";
        sqlite3_stmt* st=nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
        std::string s = std::to_string(std::max(0, rate));
        sqlite3_bind_text(st, 1, s.c_str(), -1, SQLITE_TRANSIENT);
        bool ok = (sqlite3_step(st) == SQLITE_DONE);
        sqlite3_finalize(st);
        return ok;
    }

    // CSV export/import -------------------------------------------------------
    bool exportCsv(const std::string& path) {
        std::ofstream out(path, std::ios::trunc);
        if (!out) return false;
        out << "id,title,author,totalPages,currentPage,status,isbn\n";
        auto rows = list(std::nullopt);
        for (const auto& b: rows) {
            out << b.id << ","
                << csvQuote(b.title) << ","
                << csvQuote(b.author) << ","
                << b.totalPages << ","
                << b.currentPage << ","
                << b.status << ","
                << csvQuote(b.isbn) << "\n";
        }
        return true;
    }
    bool importCsv(const std::string& path) {
        std::ifstream in(path);
        if (!in) return false;
        std::string line;
        // skip header if present
        if (!std::getline(in, line)) return false;
        // naive header check
        if (line.find("id,") == std::string::npos) {
            // no header; handle it as data
            in.clear();
            in.seekg(0);
        }

        // transactional import
        exec("BEGIN TRANSACTION;");
        while (std::getline(in, line)) {
            auto cols = csvParse(line);
            if (cols.size() < 7) continue;
            Book b;
            // id column is ignored on insert (AUTOINCREMENT)
            b.title       = cols[1];
            b.author      = cols[2];
            b.totalPages  = std::max(0, strToIntSafe(cols[3]));
            b.currentPage = std::clamp(strToIntSafe(cols[4]), 0, b.totalPages);
            b.status      = std::clamp(strToIntSafe(cols[5]), 0, 2);
            b.isbn        = cols[6];
            add(b);
        }
        exec("COMMIT;");
        return true;
    }

private:
    sqlite3* db_ = nullptr;

    void exec(const char* sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            if (err) { std::cerr << "SQLite error: " << err << "\n"; sqlite3_free(err); }
        }
    }

    static int strToIntSafe(const std::string& s) {
        try { return std::stoi(s); } catch (...) { return 0; }
        return 0;
    }

    static std::string csvQuote(const std::string& s) {
        std::string out; out.reserve(s.size()+4);
        out.push_back('"');
        for (char c: s) {
            if (c == '"') out += "\"\"";
            else out.push_back(c);
        }
        out.push_back('"');
        return out;
    }
    static std::vector<std::string> csvParse(const std::string& line) {
        std::vector<std::string> cols;
        std::string cur;
        bool inQ=false;
        for (size_t i=0;i<line.size();++i) {
            char c=line[i];
            if (inQ) {
                if (c=='"') {
                    if (i+1<line.size() && line[i+1]=='"') { cur.push_back('"'); ++i; }
                    else inQ=false;
                } else cur.push_back(c);
            } else {
                if (c==',') { cols.push_back(cur); cur.clear(); }
                else if (c=='"') inQ=true;
                else cur.push_back(c);
            }
        }
        cols.push_back(cur);
        return cols;
    }
};

// ----------------------------- UI / printing -------------------------------
static void printHeader() {
    std::cout << "\nID   "
              << std::left << std::setw(35) << "Title"
              << std::left << std::setw(22) << "Author"
              << std::left << std::setw(13) << "Progress"
              << std::left << std::setw(9)  << "% Done"
              << std::left << std::setw(9)  << "ETA"
              << std::left << std::setw(10) << "Status"
              << std::left << std::setw(15) << "ISBN"
              << "\n";
    std::cout << std::string(120, '-') << "\n";
}
static void printRow(const Book& b, int dailyRate) {
    std::cout << std::left << std::setw(5)  << b.id
              << std::left << std::setw(35) << (b.title.size()>33 ? b.title.substr(0,33)+"…" : b.title)
              << std::left << std::setw(22) << (b.author.size()>20 ? b.author.substr(0,20)+"…" : b.author)
              << std::right<< std::setw(7)  << b.currentPage << "/"
              << std::left << std::setw(6)  << b.totalPages
              << std::right<< std::setw(6)  << std::fixed << std::setprecision(1) << percentComplete(b)
              << std::left << std::setw(3)  << "%"
              << std::left << std::setw(6)  << ([&]{
                    auto d = daysToFinish(b, dailyRate);
                    if (!d) return std::string("-");
                    return std::to_string(*d) + " d";
                 })()
              << std::left << std::setw(10) << statusToStr(static_cast<Status>(b.status))
              << std::left << std::setw(15) << (b.isbn.empty()? "-" : b.isbn)
              << "\n";
}

// ----------------------------- Flows ---------------------------------------
static void listBooks(SqliteStorage& db, std::optional<Status> filter, int dailyRate) {
    printHeader();
    auto rows = filter ? db.list(static_cast<int>(*filter)) : db.list(std::nullopt);
    if (rows.empty()) { std::cout << "(no books)\n"; return; }
    for (const auto& b: rows) printRow(b, dailyRate);
}

static void addManualFlow(SqliteStorage& db) {
    Book b;
    b.title       = askLine("Title:");
    b.author      = askLine("Author (optional):", true);
    b.totalPages  = askInt("Total pages (>=0):", 0, 2'000'000'000);
    b.currentPage = askInt("Current page (>=0):", 0, std::max(0, b.totalPages));
    std::cout << "Status [to-read/reading/finished] (Enter for auto): " << std::flush;
    std::string st; std::getline(std::cin, st);
    if (auto os = strToStatus(st)) b.status = static_cast<int>(*os);
    else {
        if (b.totalPages>0 && b.currentPage >= b.totalPages) b.status = static_cast<int>(Status::Finished);
        else if (b.currentPage > 0) b.status = static_cast<int>(Status::Reading);
        else b.status = static_cast<int>(Status::ToRead);
    }
    // ISBN optional
    std::string isbn = askLine("ISBN-10/13 (optional):", true);
    b.isbn = normalizeIsbn(isbn);
    int newId = db.add(b);
    if (newId>0) std::cout << "Added book with ID #" << newId << ".\n";
    else std::cout << "Add failed.\n";
}

static void addIsbnFlow(SqliteStorage& db) {
    std::string raw = askLine("Enter ISBN-10/13:");
    std::string isbn13 = normalizeIsbn(raw);
    if (isbn13.empty()) { std::cout << "Invalid ISBN.\n"; return; }

    std::cout << "Looking up…\n";
    auto lr = lookupIsbn(isbn13);
    Book b;
    b.isbn = isbn13;

    if (lr) {
        std::cout << "Found:\n";
        std::cout << "Title:  " << (lr->title.empty()? "(unknown)" : lr->title) << "\n";
        std::cout << "Author: " << (lr->author.empty()? "(unknown)" : lr->author) << "\n";
        b.title  = lr->title;
        b.author = lr->author;
    } else {
        std::cout << "No metadata found; entering manually.\n";
    }

    if (b.title.empty())  b.title  = askLine("Title:");
    if (b.author.empty()) b.author = askLine("Author (optional):", true);

    b.totalPages  = askInt("Total pages (>=0):", 0, 2'000'000'000);
    b.currentPage = askInt("Current page (>=0):", 0, std::max(0, b.totalPages));

    std::cout << "Status [to-read/reading/finished] (Enter for auto): " << std::flush;
    std::string st; std::getline(std::cin, st);
    if (auto os = strToStatus(st)) b.status = static_cast<int>(*os);
    else {
        if (b.totalPages>0 && b.currentPage >= b.totalPages) b.status = static_cast<int>(Status::Finished);
        else if (b.currentPage > 0) b.status = static_cast<int>(Status::Reading);
        else b.status = static_cast<int>(Status::ToRead);
    }

    int newId = db.add(b);
    if (newId>0) std::cout << "Added book with ID #" << newId << ".\n";
    else std::cout << "Add failed.\n";
}

static void updatePageFlow(SqliteStorage& db) {
    int id = askInt("Book ID:", 1, INT_MAX);
    auto ob = db.get(id);
    if (!ob) { std::cout << "Not found.\n"; return; }
    std::cout << "Current: " << ob->currentPage << "/" << ob->totalPages << "\n";
    int page = askInt("Set current page:", 0, std::max(0, ob->totalPages));
    int status = ob->status;
    if (ob->totalPages>0 && page >= ob->totalPages) status = static_cast<int>(Status::Finished);
    else if (page > 0) status = static_cast<int>(Status::Reading);
    else status = static_cast<int>(Status::ToRead);
    if (db.updateProgress(id, page, status)) std::cout << "Updated.\n";
    else std::cout << "Update failed.\n";
}

static void markStatusFlow(SqliteStorage& db) {
    int id = askInt("Book ID:", 1, INT_MAX);
    auto ob = db.get(id);
    if (!ob) { std::cout << "Not found.\n"; return; }
    std::cout << "Set status: (0) To-Read  (1) Reading  (2) Finished\n";
    int s = askInt("Choice:", 0, 2);
    if (db.updateStatus(id, s)) std::cout << "Status updated.\n";
    else std::cout << "Update failed.\n";
}

static void deleteFlow(SqliteStorage& db) {
    int id = askInt("Book ID to delete:", 1, INT_MAX);
    if (db.remove(id)) std::cout << "Deleted.\n";
    else std::cout << "Not found.\n";
}

static void searchFlow(SqliteStorage& db, int dailyRate) {
    std::string q = askLine("Search title/author substring:");
    // lower the query for LIKE lower(...)
    std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c){return std::tolower(c);});
    auto matches = db.search(q);
    if (matches.empty()) { std::cout << "No matches.\n"; return; }
    printHeader();
    for (const auto& b: matches) printRow(b, dailyRate);
}

// ----------------------------- main ----------------------------------------
int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    // init curl once per process
    curl_global_init(CURL_GLOBAL_DEFAULT);

    SqliteStorage db("books.db");
    if (!db.ok()) {
        std::cerr << "Failed to open books.db\n";
        curl_global_cleanup();
        return 1;
    }

    // ------- Startup diagnostics -------
    std::cout << "\nRunning startup checks…\n";

    bool netOK   = internetOk();                   printStep("Connecting to the internet…", netOK);
    bool keyOK   = googleKeyPresent();             printStep("Getting the Google API key…", keyOK);

    bool gapiOK  = false;
    if (netOK && keyOK) {
        gapiOK = googleBooksReady();               printStep("Contacting Google Books API…", gapiOK);
    } else {
        printStep("Contacting Google Books API…", false);
    }

    bool olOK    = netOK && openLibraryOk();       printStep("Connecting to Open Library…", olOK);

    // Decide how to proceed
    g_useGoogleBooks = gapiOK; // use Google Books this run only if it’s truly ready

    if (!netOK) {
        std::cout << "\nNo internet connection. You can continue but ISBN lookup will be manual.\n";
    } else if (!gapiOK && !olOK) {
        std::cout << "\nNeither Google Books nor Open Library is reachable right now.\n"
                    "You can continue without online lookup, or exit and fix your network.\n";
    } else if (!gapiOK && olOK) {
        std::cout << "\nGoogle Books is not ready (key/network). Open Library is available.\n";
        std::cout << "1) Exit now and fix\n"
                    "2) Continue with Open Library only\n"
                    "Choice: " << std::flush;
        std::string ans; std::getline(std::cin, ans);
        if (!ans.empty() && ans[0]=='1') { std::cout << "Bye!\n"; return 0; }
        g_useGoogleBooks = false;
    }

    int dailyRate = db.getDailyRate();
    while (true) {
        std::cout << "\n====== Book Tracer (SQLite) ======\n"
                  << "1) List books\n"
                  << "2) Add book (manual)\n"
                  << "3) Add book (ISBN-10/13 + lookup)\n"
                  << "4) Update current page\n"
                  << "5) Mark status (To-Read / Reading / Finished)\n"
                  << "6) Delete book\n"
                  << "7) Search\n"
                  << "8) List with filter\n"
                  << "9) Set daily reading rate (pages/day) [current: " << dailyRate << "]\n"
                  << "10) Export CSV\n"
                  << "11) Import CSV\n"
                  << "12) Exit\n"
                  << "Choice: " << std::flush;

        std::string s; if (!std::getline(std::cin, s)) break;
        int choice = 0; try { choice = std::stoi(s); } catch (...) { choice = 0; }

        switch (choice) {
            case 1: listBooks(db, std::nullopt, dailyRate); break;
            case 2: addManualFlow(db); break;
            case 3: addIsbnFlow(db); break;
            case 4: updatePageFlow(db); break;
            case 5: markStatusFlow(db); break;
            case 6: deleteFlow(db); break;
            case 7: searchFlow(db, dailyRate); break;
            case 8: {
                std::cout << "Filter: (0) All  (1) To-Read  (2) Reading  (3) Finished\n";
                int c = askInt("Choice:", 0, 3);
                if (c==0) listBooks(db, std::nullopt, dailyRate);
                else if (c==1) listBooks(db, Status::ToRead, dailyRate);
                else if (c==2) listBooks(db, Status::Reading, dailyRate);
                else listBooks(db, Status::Finished, dailyRate);
                break;
            }
            case 9: {
            int newRate = askInt("Pages/day:", 0, 2'000'000'000);
            dailyRate = newRate;
            if (db.setDailyRate(newRate)) std::cout << "Saved.\n";
            else std::cout << "Could not save (still using new rate for this session).\n";
            break;
            }

            case 10: {
                std::string path = askLine("Export CSV path (e.g., books.csv):");
                if (db.exportCsv(path)) std::cout << "Exported.\n";
                else std::cout << "Export failed.\n";
                break;
            }
            case 11: {
                std::string path = askLine("Import CSV path:");
                if (db.importCsv(path)) std::cout << "Imported.\n";
                else std::cout << "Import failed.\n";
                break;
            }
            case 12:
                std::cout << "Bye!\n";
                curl_global_cleanup();
                return 0;
            default:
                std::cout << "Invalid choice.\n"; break;
        }
    }

    curl_global_cleanup();
    return 0;
}
