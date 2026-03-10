#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <cstring>
#include <string>

#include "crow.h"

// Добавляет полученные от libcurl байты в строку-буфер; возвращает размер записанных байт (требование API curl).
static size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// Отправляет текст пользователя в Gemini и возвращает либо ответ ассистента, либо JSON с полем error при сбое.
std::string callGemini(const std::string& apiKey, const std::string& userText) {
    nlohmann::json part;
    part["text"] = userText;
    nlohmann::json content;
    content["parts"] = nlohmann::json::array({part});
    nlohmann::json body;
    body["contents"] = nlohmann::json::array({content});

    const std::string jsonBody = body.dump();
    
    // Подключаем версию 3.1-flash
    const std::string url =
        "https://generativelanguage.googleapis.com/v1beta/models/gemini-3-flash-preview:generateContent?key=" + apiKey;

    CURL* curl = curl_easy_init();
    if (!curl) {
        return R"({"error":"curl_easy_init failed"})";
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

    const CURLcode code = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        nlohmann::json err;
        err["error"] = curl_easy_strerror(code);
        return err.dump();
    }

    try {
        const nlohmann::json j = nlohmann::json::parse(responseBody);
        if (j.contains("error")) {
            return responseBody;
        }
        if (!j.contains("candidates") || j["candidates"].empty()) {
            return R"({"error":"empty candidates"})";
        }
        const auto& cand = j["candidates"][0];
        if (!cand.contains("content") || !cand["content"].contains("parts") || cand["content"]["parts"].empty()) {
            return R"({"error":"no text in response"})";
        }
        const std::string text = cand["content"]["parts"][0].value("text", "");
        nlohmann::json ok;
        ok["reply"] = text;
        return ok.dump();
    } catch (...) {
        return R"({"error":"invalid JSON from Gemini"})";
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
            res.body = callGemini(key, userMsg);
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