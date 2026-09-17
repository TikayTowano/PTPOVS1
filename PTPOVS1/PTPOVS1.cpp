#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <cwctype> // Нужен для работы с русскими буквами (iswalnum, towlower)

int main() {
    // Включаем поддержку русского языка в консоли и функциях
    setlocale(LC_ALL, "Russian");

    // Открываем файл. Обратите внимание: файл должен быть сохранен в кодировке ANSI (Windows-1251)
    std::ifstream file("War_And_Peace.txt");
    if (!file.is_open()) {
        std::cout << "Файл не найден!" << std::endl;
        return 1;
    }

    // Словарь: "русское слово" -> "сколько раз встретилось"
    std::map<std::string, int> wordCount;
    std::string line;

    // Читаем файл построчно
    while (std::getline(file, line)) {
        
        // Превращаем строку в поток для разбивки на слова
        std::stringstream ss(line);
        std::string word;

        // Вытаскиваем слова до пробела
        while (ss >> word) {
            
            std::string cleanWord = "";
            
            // Пробегаемся по каждой букве слова
            for (char ch : word) {  
                // Переводим символ в специальный тип для проверки русской буквы
                unsigned char uc = (unsigned char)ch;
                
                // Проверяем, буква ли это (работает и для русских букв)
                if (isalpha(uc)) {
                    // Делаем букву маленькой и сохраняем
                    cleanWord += (char)tolower(uc);
                }
            }

            // Если слово не пустое, добавляем в статистику
            if (!cleanWord.empty()) {
                wordCount[cleanWord]++;
            }
        }
    }
    file.close(); // Закрываем файл

    // Выводим результат «слово - счётчик» в консоль
    for (auto pair : wordCount) {
        std::cout << pair.first << " - " << pair.second << std::endl;
    }

    return 0;
}