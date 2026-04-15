#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <cstring>
#include <iostream>
#include <string>

#include "crow_all.h"

// Добавляет полученные от libcurl байты в строку-буфер; возвращает размер записанных байт (требование API curl).
static size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// Отправляет текст пользователя в Gemini и возвращает текст ответа ассистента; при сбое возвращает пустую строку.
std::string callGemini(const std::string& apiKey, const std::string& userText) {
    const std::string systemPrompt =
        "Ты — психолог в методе КПТ. Валидируй чувства, задавай наводящие вопросы, помогай найти ошибки мышления. Отвечай кратко (2-3 предложения) на русском.";

    const std::string promptText = systemPrompt + "\n" + userText;

    nlohmann::json part;
    part["text"] = promptText;
    nlohmann::json content;
    content["parts"] = nlohmann::json::array({part});
    nlohmann::json requestBody;
    requestBody["contents"] = nlohmann::json::array({content});

    const std::string jsonBody = requestBody.dump();
    
    // Подключаем версию 3.1-flash
    const std::string url =
        "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.1-flash-lite-preview:generateContent?key=" + apiKey;

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "[Gemini CURL Error] curl_easy_init failed" << std::endl;
        return "";
    }

    std::string responseBody;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    // Прокси через кф для обхода санкций
    curl_easy_setopt(curl, CURLOPT_PROXY, "socks5h://127.0.0.1:40000");
    // Таймаут 60 секунд
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    // Таймаут на ПОДКЛЮЧЕНИЕ (15 сек)
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    // Логи
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    const CURLcode code = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        std::cerr << "[Gemini CURL Error] " << curl_easy_strerror(code) << std::endl;
        return "";
    }

    std::cerr << "--- Gemini Response Raw: " << responseBody << std::endl;
    try {
        const nlohmann::json j = nlohmann::json::parse(responseBody);
        if (j.contains("error")) {
            std::cerr << "[Gemini JSON Error] " << j["error"] << std::endl;
            return "";
        }
        if (!j.contains("candidates") || j["candidates"].empty()) {
            std::cerr << "[Gemini JSON Error] empty candidates" << std::endl;
            return "";
        }
        const auto& cand = j["candidates"][0];
        if (!cand.contains("content") || !cand["content"].contains("parts") || cand["content"]["parts"].empty()) {
            std::cerr << "[Gemini JSON Error] no text in response" << std::endl;
            return "";
        }
        const std::string text = cand["content"]["parts"][0].value("text", "");
        if (text.empty()) {
            std::cerr << "[Gemini JSON Error] empty parts[0].text" << std::endl;
        }
        return text;
    } catch (const std::exception& e) {
        std::cerr << "[Gemini JSON Parse Error] " << e.what() << std::endl;
        return "";
    }
}

int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    crow::SimpleApp app;

    CROW_ROUTE(app, "/chat")
        .methods(crow::HTTPMethod::Post)([](const crow::request& req) {
            crow::response res;
            res.add_header("Access-Control-Allow-Origin", "*");
            res.add_header("Content-Type", "application/json; charset=utf-8");

            const char* key = std::getenv("GEMINI_API_KEY");
            if (key == nullptr || std::strlen(key) == 0) {
                res.code = 500;
                res.body = R"({"error":"set GEMINI_API_KEY environment variable"})";
                return res;
            }

            const nlohmann::json in = nlohmann::json::parse(req.body, nullptr, false);
            if (!in.is_object() || !in.contains("message") || !in["message"].is_string()) {
                res.code = 400;
                res.body = R"({"error":"JSON body must be {\"message\":\"...\"}"})";
                return res;
            }

            const std::string userMsg = in["message"].get<std::string>();
            if (userMsg.empty()) {
                res.code = 400;
                res.body = R"({"error":"message is empty"})";
                return res;
            }

            std::string replyText;

            replyText = callGemini(key, userMsg);
            if (replyText.empty()) {
                res.code = 500;
                res.body = R"({"error":"Gemini request failed"})";
                std::cerr << "[Gemini Failed] replyText is empty -> HTTP 500" << std::endl;
                return res;
            }

            // Собираем JSON ответ
            crow::json::wvalue out;
            
            // Crow позволяет создавать вложенность прямо через оператор []
            // Это самый безопасный способ, который не вызывает ошибок копирования
            out["candidates"][0]["content"]["parts"][0]["text"] = replyText;

            // Вместо crow::json::dump(out) используем метод .dump() самого объекта
            res.body = out.dump(); 
            return res;
        });

    CROW_ROUTE(app, "/chat").methods(crow::HTTPMethod::Options)([] {
        crow::response res;
        res.code = 204;
        res.add_header("Access-Control-Allow-Origin", "*");
        res.add_header("Access-Control-Allow-Methods", "POST, OPTIONS");
        res.add_header("Access-Control-Allow-Headers", "Content-Type");
        return res;
    });

    app.port(8080).multithreaded().run();

    curl_global_cleanup();
    return 0;
}