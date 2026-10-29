// =====================================================================
// main.cpp — Лабораторная: контейнеры STL + JSON-локализация + UTF-8
// Сборка: VS 2026, Console App (C++), стандарт ISO C++17 или новее.
// Зависимости:
//   - nlohmann/json (json.hpp), положить в папку include/nlohmann/
//   - файлы lang/ru.json и lang/en.json — рядом с .exe (UTF-8 без BOM)
//   - War_And_Peace.txt (UTF-8) — рядом с .exe
// =====================================================================

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <vector>
#include <set>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <iterator>
#include <windows.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace std;

// =====================================================================
// ЛОКАЛИЗАЦИЯ (JSON + Win32 API)
// =====================================================================
class LocalizationManager {
    json translations;
    string currentLang;

    string detectLanguage() const {
        return (GetUserDefaultUILanguage() == 0x0419) ? "ru" : "en";
    }

public:
    LocalizationManager() {
        currentLang = detectLanguage();
        loadLanguage(currentLang);
    }

    void loadLanguage(const string& langCode) {
        string filename = "lang/" + langCode + ".json";
        ifstream file(filename, ios::binary);
        if (!file.is_open()) {
            filename = "lang/en.json";
            file.open(filename, ios::binary);
        }
        if (!file.is_open()) {
            cerr << "FATAL: cannot open language files (lang/*.json)\n";
            exit(1);
        }

        string content((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());
        // пропускаем BOM, если он есть
        if (content.size() >= 3 &&
            (unsigned char)content[0] == 0xEF &&
            (unsigned char)content[1] == 0xBB &&
            (unsigned char)content[2] == 0xBF) {
            content.erase(0, 3);
        }
        translations = json::parse(content);
        currentLang = langCode;
    }

    string tr(const string& key) const {
        if (translations.contains(key))
            return translations[key].get<string>();
        return "[" + key + "]";
    }

    void checkAndReload() {
        string detected = detectLanguage();
        if (detected != currentLang) loadLanguage(detected);
    }
};

LocalizationManager loc;

// =====================================================================
// ГЛОБАЛЬНЫЙ БУФЕР ВЫВОДА
// =====================================================================
ostringstream g_output;

// =====================================================================
// UTF-8 УТИЛИТЫ
// =====================================================================
const size_t CHUNK_SIZE = 5 * 1024 * 1024;   // 5 МБ

// Сколько байт в конце строки — незавершённая UTF-8-последовательность
static size_t incompleteUtf8Tail(const string& s) {
    size_t len = s.size();
    if (len == 0) return 0;
    size_t maxBack = std::min<size_t>(len, 3);
    for (size_t back = 1; back <= maxBack; ++back) {
        unsigned char c = (unsigned char)s[len - back];
        if ((c & 0xC0) == 0x80) continue;         // continuation — идём назад
        size_t need;
        if (c < 0x80)                need = 1;
        else if ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        else return 0;                             // битый lead
        return (back < need) ? back : 0;
    }
    return 0;
}

// Декодировать один UTF-8-символ из позиции pos
static wchar_t decodeUtf8Char(const string& s, size_t pos, size_t& consumed) {
    unsigned char c = (unsigned char)s[pos];
    if (c < 0x80) { consumed = 1; return (wchar_t)c; }
    if ((c & 0xE0) == 0xC0 && pos + 1 < s.size()) {
        consumed = 2;
        return (wchar_t)(((c & 0x1F) << 6) | ((unsigned char)s[pos + 1] & 0x3F));
    }
    if ((c & 0xF0) == 0xE0 && pos + 2 < s.size()) {
        consumed = 3;
        return (wchar_t)(((c & 0x0F) << 12)
            | (((unsigned char)s[pos + 1] & 0x3F) << 6)
            | ((unsigned char)s[pos + 2] & 0x3F));
    }
    if ((c & 0xF8) == 0xF0 && pos + 3 < s.size()) {
        consumed = 4; return L'\0';               // emoji и прочее — не буква
    }
    consumed = 1; return L'\0';                   // битый байт
}

// Буква ли это — для любого языка, без setlocale
static bool isLetterW(wchar_t wc) {
    WORD type = 0;
    return GetStringTypeW(CT_CTYPE1, &wc, 1, &type) && (type & C1_ALPHA);
}

// Привести wchar_t к нижнему регистру (Unicode-aware)
static wchar_t toLowerW(wchar_t wc) {
    WCHAR buf[2] = { wc, 0 };
    CharLowerW(buf);
    return buf[0];
}

// Дописать wchar_t в строку как UTF-8
static void appendUtf8(string& out, wchar_t wc) {
    unsigned int c = (unsigned int)wc;
    if (c < 0x80) {
        out += (char)c;
    }
    else if (c < 0x800) {
        out += (char)(0xC0 | (c >> 6));
        out += (char)(0x80 | (c & 0x3F));
    }
    else {
        out += (char)(0xE0 | (c >> 12));
        out += (char)(0x80 | ((c >> 6) & 0x3F));
        out += (char)(0x80 | (c & 0x3F));
    }
}

// =====================================================================
// ЧТЕНИЕ ФАЙЛА ПАЧКАМИ ПО 5 МБ С КОРРЕКТНЫМ UTF-8
// =====================================================================
// cb(word) вызывается для каждого слова (уже в нижнем регистре, в UTF-8).
// countChunks — сюда пишется число реально прочитанных 5-МБ-блоков.
template<typename F>
bool processFileWords(const string& filename, F cb, int* countChunks = nullptr) {
    ifstream file(filename, ios::binary);
    if (!file.is_open()) return false;

    vector<char> chunk(CHUNK_SIZE);
    string carry;          // неполная UTF-8-последовательность с прошлого блока
    string currentWord;    // накапливаем слово в UTF-8
    int chunksRead = 0;

    while (true) {
        file.read(chunk.data(), (streamsize)chunk.size());
        streamsize n = file.gcount();

        if (n > 0) ++chunksRead;

        if (n <= 0 && carry.empty()) break;

        // склеиваем прочитанное с хвостом предыдущего блока
        string data;
        data.reserve(carry.size() + (size_t)(n > 0 ? n : 0));
        data += carry;
        if (n > 0) data.append(chunk.data(), (size_t)n);
        carry.clear();

        size_t tail = incompleteUtf8Tail(data);
        size_t processLen = data.size() - tail;

        size_t pos = 0;
        while (pos < processLen) {
            size_t consumed = 0;
            wchar_t wc = decodeUtf8Char(data, pos, consumed);
            if (isLetterW(wc)) {
                appendUtf8(currentWord, toLowerW(wc));
            }
            else if (!currentWord.empty()) {
                cb(currentWord);
                currentWord.clear();
            }
            pos += consumed;
        }

        carry = data.substr(processLen);
        if (n <= 0) break;
    }

    if (!currentWord.empty()) cb(currentWord);

    if (countChunks) *countChunks = chunksRead;
    return true;
}

// =====================================================================
// ЗАДАНИЕ 1: ПОДСЧЁТ УНИКАЛЬНЫХ СЛОВ
// =====================================================================
map<string, int> countUniqueWords(const string& fn, int* chunksOut = nullptr) {
    map<string, int> m;
    if (!processFileWords(fn, [&](const string& w) { m[w]++; }, chunksOut))
        g_output << "File not found: " << fn << "\n";
    return m;
}

void printWordCount(const map<string, int>& m) {
    g_output << loc.tr("word_count_header") << "\n";
    for (const auto& [w, c] : m)
        g_output << w << " - " << c << "\n";
    g_output << "\n";
}

// =====================================================================
// ЗАДАНИЕ 2: ИНДЕКСАЦИЯ ПОЗИЦИЙ СЛОВ
// =====================================================================
map<string, vector<int>> indexWordPositions(const string& fn, int* chunksOut = nullptr) {
    map<string, vector<int>> m;
    int pos = 0;
    if (!processFileWords(fn, [&](const string& w) { m[w].push_back(pos++); }, chunksOut))
        g_output << "File not found: " << fn << "\n";
    return m;
}

void printWordIndex(const map<string, vector<int>>& m) {
    g_output << loc.tr("word_index_header") << "\n";
    for (const auto& [w, v] : m) {
        g_output << w << " – ";
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) g_output << ", ";
            g_output << v[i];
        }
        g_output << "\n";
    }
    g_output << "\n";
}

// =====================================================================
// ЗАДАНИЕ 3: STL-АЛГОРИТМЫ
// =====================================================================
bool isPrime(int n) {
    if (n <= 1) return false;
    if (n <= 3) return true;
    if (n % 2 == 0 || n % 3 == 0) return false;
    for (int i = 5; i * i <= n; i += 6)
        if (n % i == 0 || n % (i + 2) == 0) return false;
    return true;
}

// 3a: простые числа возвести в квадрат
void processPrimes(vector<int>& v) {
    g_output << loc.tr("stl_prime_header") << "\n";
    g_output << loc.tr("original_vector");
    for (int n : v) g_output << n << " ";
    g_output << "\n";

    transform(v.begin(), v.end(), v.begin(),
        [](int n) { return isPrime(n) ? n * n : n; });

    g_output << loc.tr("result_vector");
    for (int n : v) g_output << n << " ";
    g_output << "\n\n";
}

// 3b: нечётные по возрастанию, чётные по убыванию
void sortOddEven(vector<int>& v) {
    g_output << loc.tr("stl_sort_header") << "\n";
    g_output << loc.tr("original_vector");
    for (int n : v) g_output << n << " ";
    g_output << "\n";

    auto mid = partition(v.begin(), v.end(), [](int n) { return n % 2 != 0; });
    sort(v.begin(), mid);
    sort(mid, v.end(), greater<int>());

    g_output << loc.tr("result_vector");
    for (int n : v) g_output << n << " ";
    g_output << "\n\n";
}

// 3c: уникальные числа в диапазоне
vector<int> findUniqueInRange(const vector<int>& v, int low, int high) {
    g_output << loc.tr("stl_search_header") << "\n";
    set<int> s;
    for (int n : v)
        if (n >= low && n <= high) s.insert(n);
    return vector<int>(s.begin(), s.end());
}

// =====================================================================
// MAIN
// =====================================================================
int main() {
    SetConsoleOutputCP(CP_UTF8);
    // setlocale больше не нужен — классификация букв идёт через Win32 API

    // ---- 1. Ввод диапазона ДО захвата мьютекса ----
    cout << loc.tr("input_range_prompt") << flush;
    int low = 0, high = 100;
    if (!(cin >> low >> high)) { low = 0; high = 100; }

    // ---- 2. Захватываем мьютекс на всё время работы ----
    HANDLE hMutex = CreateMutexA(NULL, FALSE, "Global\\MyLabFileMutex");
    if (!hMutex) {
        cerr << "CreateMutex failed: " << GetLastError() << "\n";
        return 1;
    }

    cout << "[PID " << GetCurrentProcessId()
        << "] waiting for previous run to finish...\n" << flush;

    auto tProgramStart = chrono::high_resolution_clock::now();
    WaitForSingleObject(hMutex, INFINITE);
    auto tLockAcquired = chrono::high_resolution_clock::now();

    cout << "[PID " << GetCurrentProcessId()
        << "] got the lock, working...\n" << flush;

    // ---- 3. Вся работа, весь вывод — в g_output ----
    using Clock = chrono::high_resolution_clock;
    auto tMainStart = Clock::now();

    g_output << loc.tr("app_title") << "\n";
    g_output << "PID: " << GetCurrentProcessId() << "\n\n";

    const string textFile = "War_And_Peace.txt";

    // --- Задание 1 (с замером и счётчиком блоков) ---
    int chunks1 = 0;
    auto a = Clock::now();
    auto wordCount = countUniqueWords(textFile, &chunks1);
    auto b = Clock::now();
    long long tCountMs = chrono::duration_cast<chrono::milliseconds>(b - a).count();
    printWordCount(wordCount);
    g_output << "[read chunks for task 1: " << chunks1 << " x up to 5 MB]\n\n";

    // --- Задание 2 (с замером и счётчиком блоков) ---
    int chunks2 = 0;
    a = Clock::now();
    auto wordPositions = indexWordPositions(textFile, &chunks2);
    b = Clock::now();
    long long tIndexMs = chrono::duration_cast<chrono::milliseconds>(b - a).count();
    printWordIndex(wordPositions);
    g_output << "[read chunks for task 2: " << chunks2 << " x up to 5 MB]\n\n";

    // --- Задание 3a ---
    vector<int> testVec = { 12, 7, 9, 4, 15, 2, 8, 3, 11, 6, 13, 10 };
    a = Clock::now();
    processPrimes(testVec);
    b = Clock::now();
    long long tPrimeMs = chrono::duration_cast<chrono::milliseconds>(b - a).count();

    // --- Задание 3b ---
    a = Clock::now();
    sortOddEven(testVec);
    b = Clock::now();
    long long tSortMs = chrono::duration_cast<chrono::milliseconds>(b - a).count();

    // --- Задание 3c ---
    a = Clock::now();
    auto uniqueNums = findUniqueInRange(testVec, low, high);
    b = Clock::now();
    long long tSearchMs = chrono::duration_cast<chrono::milliseconds>(b - a).count();

    g_output << loc.tr("result_vector");
    for (int n : uniqueNums) g_output << n << " ";
    g_output << "\n\n";

    // ---- 4. Тайминги ----
    auto tWorkEnd = Clock::now();
    auto waitMs = chrono::duration_cast<chrono::milliseconds>(tLockAcquired - tProgramStart).count();
    auto workMs = chrono::duration_cast<chrono::milliseconds>(tWorkEnd - tLockAcquired).count();
    auto totalMs = chrono::duration_cast<chrono::milliseconds>(tWorkEnd - tProgramStart).count();
    auto mainMs = chrono::duration_cast<chrono::milliseconds>(tWorkEnd - tMainStart).count();

    g_output << "=== TIMING ===\n";
    g_output << "  countUniqueWords   : " << tCountMs << " ms\n";
    g_output << "  indexWordPositions : " << tIndexMs << " ms\n";
    g_output << "  processPrimes      : " << tPrimeMs << " ms\n";
    g_output << "  sortOddEven        : " << tSortMs << " ms\n";
    g_output << "  findUniqueInRange  : " << tSearchMs << " ms\n";
    g_output << "  ---\n";
    g_output << "  main() work        : " << mainMs << " ms\n";
    g_output << "  wait for mutex     : " << waitMs << " ms\n";
    g_output << "  work (locked)      : " << workMs << " ms\n";
    g_output << "  TOTAL (start->end) : " << totalMs << " ms\n";
    g_output << "=== run finished ===\n\n";

    // ---- 5. Сброс в файл (append) и в консоль ----
    {
        ofstream out("output.log", ios::app | ios::binary);
        if (out.is_open()) out << g_output.str();
    }
    cout << "\n" << g_output.str() << flush;

    // ---- 6. Освобождаем мьютекс ----
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return 0;
}