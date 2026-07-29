package com.example.tablet_client

import android.media.MediaCodec
import android.media.MediaFormat
import android.view.Surface
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel
import io.flutter.view.TextureRegistry
import java.nio.ByteBuffer
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

// Нативная сторона видео-декодирования: принимает H.264 NAL-юниты от Dart
// (через MethodChannel) и декодирует их аппаратно через MediaCodec прямо
// на Surface, связанный с текстурой Flutter - без единого лишнего копирования
// пикселей на стороне Dart/Flutter.
class MainActivity : FlutterActivity() {
    private val CHANNEL = "display_translation/video"

    private var engine: FlutterEngine? = null
    private var textureEntry: TextureRegistry.SurfaceTextureEntry? = null
    private var codec: MediaCodec? = null
    private var decoderThread: Thread? = null
    @Volatile private var running = false
    private val inputQueue = LinkedBlockingQueue<ByteArray>()

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        engine = flutterEngine

        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, CHANNEL).setMethodCallHandler { call, result ->
            when (call.method) {
                "start" -> {
                    val width = call.argument<Int>("width") ?: 1280
                    val height = call.argument<Int>("height") ?: 720
                    try {
                        val textureId = startDecoder(width, height)
                        result.success(textureId)
                    } catch (e: Exception) {
                        result.error("START_FAILED", e.message, null)
                    }
                }
                "feedData" -> {
                    val data = call.argument<ByteArray>("data")
                    if (data != null && running) {
                        inputQueue.offer(data)
                    }
                    result.success(null)
                }
                "stop" -> {
                    stopDecoder()
                    result.success(null)
                }
                else -> result.notImplemented()
            }
        }
    }

    private fun startDecoder(width: Int, height: Int): Long {
        stopDecoder() // на случай переподключения с новым разрешением

        val entry = engine!!.renderer.createSurfaceTexture()
        textureEntry = entry
        val surfaceTexture = entry.surfaceTexture()
        surfaceTexture.setDefaultBufferSize(width, height)
        val surface = Surface(surfaceTexture)

        val format = MediaFormat.createVideoFormat("video/avc", width, height)
        val mediaCodec = MediaCodec.createDecoderByType("video/avc")
        mediaCodec.configure(format, surface, null, 0)
        mediaCodec.start()
        codec = mediaCodec

        running = true
        decoderThread = Thread {
            val bufferInfo = MediaCodec.BufferInfo()
            while (running) {
                try {
                    val data = inputQueue.poll(200, TimeUnit.MILLISECONDS) ?: continue
                    feedInput(mediaCodec, data)
                    drainOutput(mediaCodec, bufferInfo)
                } catch (e: Exception) {
                    // Пропускаем проблемный кадр, не роняя весь поток декодирования
                }
            }
        }
        decoderThread?.start()

        return entry.id()
    }

    private fun feedInput(mediaCodec: MediaCodec, data: ByteArray) {
        val inputIndex = mediaCodec.dequeueInputBuffer(10_000)
        if (inputIndex >= 0) {
            val inputBuffer: ByteBuffer? = mediaCodec.getInputBuffer(inputIndex)
            inputBuffer?.clear()
            inputBuffer?.put(data)
            mediaCodec.queueInputBuffer(inputIndex, 0, data.size, System.nanoTime() / 1000, 0)
        }
    }

    private fun drainOutput(mediaCodec: MediaCodec, bufferInfo: MediaCodec.BufferInfo) {
        while (true) {
            val outputIndex = mediaCodec.dequeueOutputBuffer(bufferInfo, 0)
            if (outputIndex >= 0) {
                // render = true - сразу выводит декодированный кадр на Surface (текстуру)
                mediaCodec.releaseOutputBuffer(outputIndex, true)
            } else {
                break
            }
        }
    }

    private fun stopDecoder() {
        running = false
        decoderThread?.join(500)
        decoderThread = null
        inputQueue.clear()

        try {
            codec?.stop()
            codec?.release()
        } catch (e: Exception) {
        }
        codec = null

        textureEntry?.release()
        textureEntry = null
    }

    override fun onDestroy() {
        stopDecoder()
        super.onDestroy()
    }
}
