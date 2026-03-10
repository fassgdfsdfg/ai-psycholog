import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;
import 'package:shared_preferences/shared_preferences.dart';

const String serverBaseUrl = 'http://10.0.2.2:8080';

const String _prefsKey = 'chat_history_lines';

void main() {
  runApp(const AiPsychologistApp());
}

class AiPsychologistApp extends StatelessWidget {
  const AiPsychologistApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      title: 'AI Психолог',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.teal),
        useMaterial3: true,
      ),
      home: const ChatScreen(),
    );
  }
}

class ChatScreen extends StatefulWidget {
  const ChatScreen({super.key});

  @override
  State<ChatScreen> createState() => _ChatScreenState();
}

class _ChatScreenState extends State<ChatScreen> {
  final TextEditingController _controller = TextEditingController();
  final List<String> _lines = [];
  bool _busy = false;
  bool _storageReady = false;

  @override
  void initState() {
    super.initState();
    _loadFromDevice();
  }

  // Читает JSON-массив строк из SharedPreferences и заполняет список сообщений на экране; при ошибке или пустом значении список не меняется. В конце выставляет флаг, что загрузка завершена.
  Future<void> _loadFromDevice() async {
    final prefs = await SharedPreferences.getInstance();
    final raw = prefs.getString(_prefsKey);
    if (!mounted) return;
    if (raw != null && raw.isNotEmpty) {
      try {
        final decoded = jsonDecode(raw) as List<dynamic>;
        setState(() {
          _lines
            ..clear()
            ..addAll(decoded.map((e) => e.toString()));
        });
      } catch (_) {}
    }
    if (!mounted) return;
    setState(() => _storageReady = true);
  }

  // Сохраняет текущий список строк сообщений в SharedPreferences в виде JSON-массива строк.
  Future<void> _saveToDevice() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_prefsKey, jsonEncode(_lines));
  }

  // Добавляет одну строку в историю, обновляет экран и сразу записывает историю в память устройства.
  Future<void> _appendLine(String line) async {
    setState(() => _lines.add(line));
    await _saveToDevice();
  }

  Future<void> _send() async {
    final text = _controller.text.trim();
    if (text.isEmpty || _busy || !_storageReady) return;

    setState(() => _busy = true);
    _controller.clear();
    await _appendLine('Вы: $text');

    try {
      final uri = Uri.parse('$serverBaseUrl/chat');
      final resp = await http.post(
        uri,
        headers: {'Content-Type': 'application/json; charset=utf-8'},
        body: jsonEncode({'message': text}),
      );

      if (resp.statusCode != 200) {
        await _appendLine('Ошибка сервера (${resp.statusCode}): ${resp.body}');
        return;
      }

      try {
        final map = jsonDecode(resp.body) as Map<String, dynamic>;
        if (map.containsKey('reply')) {
          await _appendLine('Бот: ${map['reply']}');
        } else {
          await _appendLine('Ошибка: ${map['error'] ?? 'Неизвестная ошибка'}');
        }
      } catch (_) {
        await _appendLine('Сырой ответ: ${resp.body}');
      }
    } catch (e) {
      await _appendLine('Ошибка сети: $e');
    } finally {
      if (mounted) {
        setState(() => _busy = false);
      }
    }
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (!_storageReady) {
      return Scaffold(
        appBar: AppBar(title: const Text('AI Психолог')),
        body: const Center(child: CircularProgressIndicator()),
      );
    }

    return Scaffold(
      appBar: AppBar(title: const Text('AI Психолог')),
      body: Column(
        children: [
          Expanded(
            child: ListView.builder(
              padding: const EdgeInsets.all(12),
              itemCount: _lines.length,
              itemBuilder: (context, i) => Padding(
                padding: const EdgeInsets.only(bottom: 8),
                child: Text(_lines[i]),
              ),
            ),
          ),
          const Divider(height: 1),
          Padding(
            padding: const EdgeInsets.fromLTRB(12, 8, 12, 12),
            child: Row(
              children: [
                Expanded(
                  child: TextField(
                    controller: _controller,
                    decoration: const InputDecoration(
                      hintText: 'Сообщение…',
                      border: OutlineInputBorder(),
                    ),
                    minLines: 1,
                    maxLines: 4,
                    textInputAction: TextInputAction.send,
                    onSubmitted: (_) => _send(),
                  ),
                ),
                const SizedBox(width: 8),
                FilledButton(
                  onPressed: (_busy || !_storageReady) ? null : _send,
                  child: _busy
                      ? const SizedBox(
                          width: 20,
                          height: 20,
                          child: CircularProgressIndicator(strokeWidth: 2),
                        )
                      : const Text('Отпр.'),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}
