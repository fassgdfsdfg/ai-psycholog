#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <cstring>
#include <iostream>
#include <string>

#include "crow_all.h"

// Для curl, чтобы он собирал ответ от Google в одну строку, требование гугла
static size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// Основная функция для работы с нейросетью. Тут формируем запрос, настраиваем прокси и парсим ответ.
std::string callGemini(const std::string& apiKey, const std::string& userText) {
    // Промпт для психолога. Важно, чтобы отвечал кратко, из за лимитов.
    const std::string systemPrompt =
            "Ты — психолог в методе КПТ. Валидируй чувства, задавай наводящие вопросы, помогай найти ошибки мышления. Отвечай кратко (2-3 предложения) на русском.";

    const std::string promptText = systemPrompt + "\n" + userText;

    // Собираем структуру запроса по докам Google Gemini
    nlohmann::json part;
    part["text"] = promptText;
    nlohmann::json content;
    content["parts"] = nlohmann::json::array({part});
    nlohmann::json requestBody;
    requestBody["contents"] = nlohmann::json::array({content});

    const std::string jsonBody = requestBody.dump();

    // Используем версию 3.1-flash-lite
    const std::string url =
            "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.1-flash-lite-preview:generateContent?key=" + apiKey;

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "[Ошибка] Curl не захотел инициализироваться" << std::endl;
        return "";
    }

    std::string responseBody;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    // Настраиваем curl: передаем заголовки, тело запроса и коллбэк для записи ответа
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);

    // Настройка прокси через WARP (порт 40000), обход санкций
    curl_easy_setopt(curl, CURLOPT_PROXY, "socks5h://127.0.0.1:40000");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L); // Ждем максимум минуту
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);

    // Выполняем запрос
    const CURLcode code = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        std::cerr << "[Ошибка CURL] Что-то с сетью: " << curl_easy_strerror(code) << std::endl;
        return "";
    }

    // Теперь надо вытащить из JSON ответа только сам текст ответа
    try {
        const nlohmann::json j = nlohmann::json::parse(responseBody);

        // Проверяем, на ошибки от Google API
        if (j.contains("error")) {
            std::cerr << "[Ошибка API] Гугл ругается: " << j["error"].dump() << std::endl;
            return "";
        }

        // Идем в объект за текстом: candidates -> content -> parts -> text
        const auto& cand = j["candidates"][0];
        const std::string text = cand["content"]["parts"][0].value("text", "");

        return text;
    } catch (const std::exception& e) {
        std::cerr << "[Ошибка парсинга] Пришел кривой JSON: " << e.what() << std::endl;
        return "";
    }
}

int main() {
    // Глобальные настройки curl
    curl_global_init(CURL_GLOBAL_DEFAULT);

    crow::SimpleApp app;

    // Сюда фронт отправляет сообщения
    CROW_ROUTE(app, "/chat")
            .methods(crow::HTTPMethod::Post)([](const crow::request& req) {
                crow::response res;
                // Разрешаем запросы откуда угодно (через CORS)
                res.add_header("Access-Control-Allow-Origin", "*");
                res.add_header("Content-Type", "application/json; charset=utf-8");

                // Берем ключ из переменной окружения
                const char* key = std::getenv("GEMINI_API_KEY");
                if (key == nullptr || std::strlen(key) == 0) {
                    res.code = 500;
                    res.body = R"({"error":"Забыл прописать GEMINI_API_KEY на сервере!"})";
                    return res;
                }

                // Достаем сообщение от пользователя
                const nlohmann::json in = nlohmann::json::parse(req.body, nullptr, false);
                if (in.is_discarded() || !in.contains("message")) {
                    res.code = 400;
                    res.body = R"({"error":"Жду JSON типа {"message": "..."}"})";
                    return res;
                }

                const std::string userMsg = in["message"].get<std::string>();

                // Получаем ответ
                std::string replyText = callGemini(key, userMsg);

                if (replyText.empty()) {
                    res.code = 500;
                    res.body = R"({"error":"Нейронка не ответила, надо проверять логи сервера"})";
                    return res;
                }

                // Упрощаем ответ для фронтенда: отправляем просто один ключ "reply"
                nlohmann::json out;
                out["reply"] = replyText;

                res.body = out.dump(); // Превращаем объект в строку и отправляем
                return res;
            });

    // Обработка префлайт-запросов
    CROW_ROUTE(app, "/chat").methods(crow::HTTPMethod::Options)([] {
        crow::response res;
        res.code = 204;
        res.add_header("Access-Control-Allow-Origin", "*");
        res.add_header("Access-Control-Allow-Methods", "POST, OPTIONS");
        res.add_header("Access-Control-Allow-Headers", "Content-Type");
        return res;
    });

    // Запускаем сервер на 8080 порту
    std::cout << "Сервер запущен. Порт 8080." << std::endl;
    app.port(8080).multithreaded().run();

    curl_global_cleanup();
    return 0;
}