import 'dart:async';
import 'dart:io';
import 'dart:typed_data';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

const int kPort = 27183;

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  SystemChrome.setEnabledSystemUIMode(SystemUiMode.immersiveSticky);
  SystemChrome.setPreferredOrientations([
    DeviceOrientation.landscapeLeft,
    DeviceOrientation.landscapeRight,
  ]);
  runApp(const DisplayTranslationApp());
}

class DisplayTranslationApp extends StatelessWidget {
  const DisplayTranslationApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: ThemeData.dark(),
      home: const ScreenMirrorPage(),
    );
  }
}

class ScreenMirrorPage extends StatefulWidget {
  const ScreenMirrorPage({super.key});

  @override
  State<ScreenMirrorPage> createState() => _ScreenMirrorPageState();
}

class _ScreenMirrorPageState extends State<ScreenMirrorPage> {
  Socket? _socket;
  Uint8List? _lastFrame;
  String _status = 'Подключение...';
  bool _connecting = false;

  // Буфер для сборки кадров из TCP-потока (данные могут прийти по частям).
  final BytesBuilder _buffer = BytesBuilder(copy: false);
  int? _expectedFrameLen;

  @override
  void initState() {
    super.initState();
    _connect();
  }

  Future<void> _connect() async {
    if (_connecting) return;
    _connecting = true;
    setState(() => _status = 'Подключение к ПК (порт $kPort)...');

    try {
      final socket = await Socket.connect('127.0.0.1', kPort,
          timeout: const Duration(seconds: 5));
      _socket = socket;
      setState(() => _status = 'Подключено');

      // Сообщаем серверу физическое разрешение экрана устройства,
      // чтобы он сам подобрал подходящий регион захвата.
      final view = WidgetsBinding.instance.platformDispatcher.views.first;
      final physicalSize = view.physicalSize;
      final w = physicalSize.width.toInt();
      final h = physicalSize.height.toInt();
      final handshake = ByteData(8);
      handshake.setUint32(0, w, Endian.little);
      handshake.setUint32(4, h, Endian.little);
      socket.add(handshake.buffer.asUint8List());

      socket.listen(
        _onData,
        onError: (_) => _onDisconnected(),
        onDone: _onDisconnected,
        cancelOnError: true,
      );
    } catch (e) {
      setState(() => _status = 'Нет соединения. Повтор через 3с...\n$e');
      _scheduleReconnect();
    } finally {
      _connecting = false;
    }
  }

  void _scheduleReconnect() {
    Future.delayed(const Duration(seconds: 3), () {
      if (mounted && _socket == null) _connect();
    });
  }

  void _onDisconnected() {
    _socket = null;
    _buffer.clear();
    _expectedFrameLen = null;
    if (!mounted) return;
    setState(() => _status = 'Соединение разорвано. Переподключение...');
    _scheduleReconnect();
  }

  void _onData(Uint8List chunk) {
    _buffer.add(chunk);

    // Собираем ВСЕ целые кадры, накопившиеся в буфере, но показываем только самый последний.
    // Это критично важно на слабых устройствах - иначе отставание будет только расти,
    // так как устройство не успевает отрисовывать все накопившиеся кадры.
    Uint8List? latestFrame;

    while (true) {
      final bytes = _buffer.toBytes();

      if (_expectedFrameLen == null) {
        if (bytes.length < 4) break;
        final bd = ByteData.sublistView(bytes, 0, 4);
        _expectedFrameLen = bd.getUint32(0, Endian.little);
        _buffer.clear();
        _buffer.add(bytes.sublist(4));
        continue;
      }

      final currentBytes = _buffer.toBytes();
      if (currentBytes.length < _expectedFrameLen!) break;

      final frame = currentBytes.sublist(0, _expectedFrameLen!);
      final rest = currentBytes.sublist(_expectedFrameLen!);
      _buffer.clear();
      _buffer.add(rest);
      _expectedFrameLen = null;

      latestFrame = frame;
    }

    if (latestFrame != null) {
      _lastFrame = latestFrame;
      if (mounted) setState(() {});
    }
  }

  @override
  void dispose() {
    _socket?.destroy();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.black,
      body: Center(
        child: _lastFrame != null
            ? Image.memory(
                _lastFrame!,
                gaplessPlayback: true,
                fit: BoxFit.contain,
              )
            : Text(
                _status,
                textAlign: TextAlign.center,
                style: const TextStyle(color: Colors.white54, fontSize: 16),
              ),
      ),
    );
  }
}
