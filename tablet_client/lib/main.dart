import 'dart:async';
import 'dart:io';
import 'dart:typed_data';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

const int kPort = 27183;
const MethodChannel _videoChannel = MethodChannel('display_translation/video');

// Простая растущая очередь байт без лишних копий. Старый код через
// BytesBuilder делал toBytes()+clear()+add(sublist) на каждой итерации разбора -
// это полная перекопировка всего непрочитанного хвоста буфера при каждом вызове, лишня
// нагрузка на GC и потенциальный источник микрофризов при высоком fps.
// Каждый байт копируется ровно один раз - когда он окончательно вошёл в собранный кадр.
class _ByteQueue {
  final List<Uint8List> _chunks = [];
  int _headOffset = 0; // сколько байт в начале первого чанка уже съедено
  int _length = 0;

  int get length => _length;

  void add(Uint8List data) {
    if (data.isEmpty) return;
    _chunks.add(data);
    _length += data.length;
  }

  // Возвращает ровно [n] байт без удаления из очереди (нужно вызвать consume() отдельно).
  Uint8List peek(int n) {
    // Быстрый путь: все нужные байты уже лежат одним куском без копирования.
    if (_chunks.isNotEmpty) {
      final first = _chunks.first;
      if (_headOffset == 0 && first.length == n) return first;
      if (first.length - _headOffset >= n) {
        return Uint8List.sublistView(first, _headOffset, _headOffset + n);
      }
    }
    final result = Uint8List(n);
    int written = 0;
    int chunkIndex = 0;
    int inChunkOffset = _headOffset;
    while (written < n) {
      final chunk = _chunks[chunkIndex];
      final available = chunk.length - inChunkOffset;
      final toCopy = (n - written) < available ? (n - written) : available;
      result.setRange(written, written + toCopy, chunk, inChunkOffset);
      written += toCopy;
      inChunkOffset += toCopy;
      if (inChunkOffset >= chunk.length) {
        chunkIndex++;
        inChunkOffset = 0;
      }
    }
    return result;
  }

  void consume(int n) {
    _length -= n;
    int remaining = n;
    while (remaining > 0) {
      final chunk = _chunks.first;
      final available = chunk.length - _headOffset;
      if (remaining < available) {
        _headOffset += remaining;
        remaining = 0;
      } else {
        remaining -= available;
        _chunks.removeAt(0);
        _headOffset = 0;
      }
    }
  }

  void clear() {
    _chunks.clear();
    _headOffset = 0;
    _length = 0;
  }
}

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

enum _ConnState { awaitingSize, streaming }

class ScreenMirrorPage extends StatefulWidget {
  const ScreenMirrorPage({super.key});

  @override
  State<ScreenMirrorPage> createState() => _ScreenMirrorPageState();
}

class _ScreenMirrorPageState extends State<ScreenMirrorPage> {
  Socket? _socket;
  int? _textureId;
  String _status = 'Подключение...';
  bool _connecting = false;
  _ConnState _connState = _ConnState.awaitingSize;

  // Буфер для сборки данных из TCP-потока (данные могут прийти по частям).
  final _ByteQueue _buffer = _ByteQueue();
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
      // Как и на стороне сервера: без этого Nagle's algorithm может задерживать
      // отправку рукопожатия/мелких пакетов на десятки мс.
      socket.setOption(SocketOption.tcpNoDelay, true);
      _socket = socket;
      _connState = _ConnState.awaitingSize;
      _buffer.clear();
      _expectedFrameLen = null;

      // Сообщаем серверу физическое разрешение экрана устройства
      final view = WidgetsBinding.instance.platformDispatcher.views.first;
      final physicalSize = view.physicalSize;
      final w = physicalSize.width.toInt();
      final h = physicalSize.height.toInt();
      final handshake = ByteData(8);
      handshake.setUint32(0, w, Endian.little);
      handshake.setUint32(4, h, Endian.little);
      socket.add(handshake.buffer.asUint8List());

      setState(() => _status = 'Подключено, ждём параметры видео...');

      // Один-единственный listen на весь срок жизни сокета (Socket - single-subscription
      // stream, повторный listen() кидает исключение - именно это ломало соединение раньше).
      socket.listen(
        _onData,
        onError: (e) => _onDisconnected('Ошибка сокета: $e'),
        onDone: () => _onDisconnected('Соединение закрыто сервером'),
        cancelOnError: true,
      );
    } catch (e) {
      setState(() => _status = 'Нет соединения. Повтор через 3с...\n$e');
      _socket = null;
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

  void _onDisconnected(String reason) {
    _socket?.destroy();
    _socket = null;
    _buffer.clear();
    _expectedFrameLen = null;
    _videoChannel.invokeMethod('stop');
    if (!mounted) return;
    setState(() {
      _textureId = null;
      _status = '$reason\nПереподключение через 3с...';
    });
    _scheduleReconnect();
  }

  void _onData(Uint8List chunk) {
    _buffer.add(chunk);

    if (_connState == _ConnState.awaitingSize) {
      if (_buffer.length < 8) return; // ждём остальные байты рукопожатия

      final header = _buffer.peek(8);
      final bd = ByteData.sublistView(header, 0, 8);
      final frameW = bd.getUint32(0, Endian.little);
      final frameH = bd.getUint32(4, Endian.little);
      _buffer.consume(8);
      _connState = _ConnState.streaming;

      _videoChannel.invokeMethod<int>('start', {
        'width': frameW,
        'height': frameH,
      }).then((textureId) {
        if (mounted) {
          setState(() {
            _textureId = textureId;
            _status = 'Подключено';
          });
        }
      }).catchError((e) {
        if (mounted) setState(() => _status = 'Ошибка запуска декодера: $e');
      });
    }

    if (_connState != _ConnState.streaming) return;

    while (true) {
      if (_expectedFrameLen == null) {
        if (_buffer.length < 4) break;
        final header = _buffer.peek(4);
        final bd = ByteData.sublistView(header, 0, 4);
        _expectedFrameLen = bd.getUint32(0, Endian.little);
        _buffer.consume(4);
        continue;
      }

      if (_buffer.length < _expectedFrameLen!) break;

      final frame = _buffer.peek(_expectedFrameLen!);
      _buffer.consume(_expectedFrameLen!);
      _expectedFrameLen = null;

      // H.264 кадры зависят друг от друга (P-фреймы) - в отличие от JPEG-версии,
      // тут НЕЛЬЗЯ пропускать кадры, все передаются нативному декодеру по порядку.
      _videoChannel.invokeMethod('feedData', {'data': frame});
    }
  }

  @override
  void dispose() {
    _socket?.destroy();
    _videoChannel.invokeMethod('stop');
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.black,
      body: Stack(
        children: [
          if (_textureId != null)
            Positioned.fill(child: Texture(textureId: _textureId!)),
          if (_textureId == null)
            Center(
              child: Padding(
                padding: const EdgeInsets.all(24),
                child: Text(
                  _status,
                  textAlign: TextAlign.center,
                  style: const TextStyle(color: Colors.white, fontSize: 20),
                ),
              ),
            ),
        ],
      ),
    );
  }
}
