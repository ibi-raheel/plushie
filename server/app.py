# ESP32 Toy Backend 123
from flask import Flask, request, send_file, jsonify
import os, datetime, time
import struct
import wave

from whisper_stt import transcribe_audio
from gpt_reply import get_gpt_reply
from tts import synthesize_speech

app = Flask(__name__)
TEMP_DIR = os.path.join(os.path.dirname(os.path.dirname(__file__)), "temp")
AUDIO_DIR = os.path.join(os.path.dirname(__file__), "audio")
os.makedirs(TEMP_DIR, exist_ok=True)
os.makedirs(AUDIO_DIR, exist_ok=True)

def decompress_adpcm_to_wav(adpcm_data, output_path, sample_rate=16000):
    """
    Decompress ADPCM data to WAV format
    Assumes IMA ADPCM format commonly used by ESP32
    """
    try:
        # IMA ADPCM step size table
        step_table = [
            7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
            19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
            50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
            130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
            337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
            876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
            2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
            5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
            15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
        ]
        
        # Index table for IMA ADPCM (matches ESP32 implementation)
        index_table = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
        
        # Initialize decoder state
        predicted_sample = 0
        step_index = 0
        decoded_samples = []
        
        # Process ADPCM data
        for byte in adpcm_data:
            # Each byte contains two 4-bit ADPCM samples
            for nibble in [(byte & 0x0F), (byte >> 4)]:
                step = step_table[step_index]
                
                # Decode the nibble (matching ESP32 encoder logic)
                diffq = step >> 3
                if nibble & 4:
                    diffq += step
                if nibble & 2:
                    diffq += step >> 1
                if nibble & 1:
                    diffq += step >> 2
                
                if nibble & 8:
                    predicted_sample -= diffq
                else:
                    predicted_sample += diffq
                
                # Clamp to 16-bit range
                predicted_sample = max(-32768, min(32767, predicted_sample))
                decoded_samples.append(predicted_sample)
                
                # Update step index
                step_index += index_table[nibble]
                step_index = max(0, min(88, step_index))
        
        # Convert to bytes (16-bit PCM)
        pcm_data = struct.pack('<' + 'h' * len(decoded_samples), *decoded_samples)
        
        # Write WAV file
        with wave.open(output_path, 'wb') as wav_file:
            wav_file.setnchannels(1)  # Mono
            wav_file.setsampwidth(2)  # 16-bit
            wav_file.setframerate(sample_rate)
            wav_file.writeframes(pcm_data)
        
        print(f"[INFO] ADPCM decompressed: {len(adpcm_data)} bytes -> {len(decoded_samples)} samples")
        return True
        
    except Exception as e:
        print(f"[ERROR] ADPCM decompression failed: {str(e)}")
        return False

@app.route("/")
def index():
    return "ESP32 Toy Backend is running. Broooo its workinggggggggggggg"

@app.route("/upload", methods=["POST"])
def upload_audio():
    # Start timing measurements
    start_time = time.time()
    timing_log = {"start": start_time}
    
    # 1. Receive audio
    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    input_path = os.path.join(TEMP_DIR, f"input_{timestamp}.wav")
    adpcm_path = os.path.join(TEMP_DIR, f"adpcm_{timestamp}.bin")

    # Get session ID from headers (ESP32 can send via X-Session-ID header)
    session_id = request.headers.get('X-Session-ID', 'esp32_audio_default')

    if "audio" in request.files:               # multipart
        # Save the uploaded file temporarily
        request.files["audio"].save(adpcm_path)
        audio_data = request.files["audio"].read()
        request.files["audio"].seek(0)  # Reset file pointer for potential re-read
        request.files["audio"].save(adpcm_path)
    else:                                      # raw body
        audio_data = request.data
        with open(adpcm_path, "wb") as f:
            f.write(audio_data)

    # Detect format and handle accordingly
    content_type = request.content_type or request.headers.get('Content-Type', '')
    
    if content_type == "audio/adpcm" or request.headers.get('X-Audio-Format') == 'adpcm':
        # ADPCM format - decompress to WAV
        print(f"[INFO] Processing ADPCM audio ({len(audio_data)} bytes) (Session: {session_id})")
        
        if not decompress_adpcm_to_wav(audio_data, input_path):
            return jsonify({"error": "ADPCM decompression failed"}), 500
            
        print(f"[INFO] ADPCM decompressed => {input_path}")
        
    elif content_type == "audio/wav":
        # Already WAV format - save directly
        with open(input_path, "wb") as f:
            f.write(audio_data)
        print(f"[INFO] Saved WAV audio => {input_path} (Session: {session_id})")
        
    else:
        # Assume ADPCM by default for ESP32 compatibility
        print(f"[INFO] No format specified, assuming ADPCM ({len(audio_data)} bytes) (Session: {session_id})")
        
        if not decompress_adpcm_to_wav(audio_data, input_path):
            return jsonify({"error": "ADPCM decompression failed"}), 500
            
        print(f"[INFO] ADPCM decompressed => {input_path}")

    timing_log["audio_saved"] = time.time()

    # 2. STT (Server-side Whisper API)
    stt_start = time.time()
    print(f"[INFO] Starting server-side STT for session: {session_id}")
    user_text = transcribe_audio(input_path)
    if not user_text:
        print("[ERROR] Server STT failed - no transcription returned")
        return jsonify({"error": "Speech transcription failed"}), 500
    timing_log["stt_complete"] = time.time()
    stt_time = timing_log["stt_complete"] - stt_start
    print(f"[TRANSCRIPT] '{user_text}' (took {stt_time:.2f}s)")

    # 3. GPT with memory context
    gpt_start = time.time()
    gpt_reply = get_gpt_reply(user_text, session_id)
    timing_log["gpt_complete"] = time.time()
    print("[GPT]", gpt_reply)

    # 4. TTS
    tts_start = time.time()
    output_path = os.path.join(TEMP_DIR, f"reply_{timestamp}.wav")
    print(f"[DEBUG] Expected output path: {os.path.abspath(output_path)}")
    synthesize_speech(gpt_reply, output_path)

    # 5. Wait for TTS output file to be created (with retry)
    max_retries = 60
    retry_delay = 1.0
    for attempt in range(max_retries):
        if os.path.exists(output_path) and os.path.getsize(output_path) > 0:
            break
        print(f"[DEBUG] Attempt {attempt + 1}: File not ready, waiting {retry_delay}s...")
        time.sleep(retry_delay)
    else:
        print(f"[DEBUG] File still not found after {max_retries} attempts at: {os.path.abspath(output_path)}")
        print(f"[DEBUG] Current working directory: {os.getcwd()}")
        print(f"[DEBUG] Files in temp directory: {os.listdir('../temp') if os.path.exists('../temp') else 'temp dir not found'}")
        return jsonify({"error": "Speech synthesis failed"}), 500

    # 6. Send back WAV with proper headers
    timing_log["tts_complete"] = time.time()
    file_size = os.path.getsize(output_path)
    
    # Calculate timing breakdown
    total_time = timing_log["tts_complete"] - timing_log["start"]
    stt_time = timing_log["stt_complete"] - timing_log["audio_saved"]
    gpt_time = timing_log["gpt_complete"] - timing_log["stt_complete"]
    tts_time = timing_log["tts_complete"] - timing_log["gpt_complete"]
    
    print(f"\n=== RESPONSE TIME ANALYSIS ===")
    print(f"Total Response Time: {total_time:.2f}s")
    print(f"├─ STT Processing: {stt_time:.2f}s ({stt_time/total_time*100:.1f}%)")
    print(f"├─ GPT Generation: {gpt_time:.2f}s ({gpt_time/total_time*100:.1f}%)")
    print(f"└─ TTS Generation: {tts_time:.2f}s ({tts_time/total_time*100:.1f}%)")
    print(f"Audio file size: {file_size} bytes")
    print(f"=== END TIMING ANALYSIS ===\n")
    
    response = send_file(output_path, mimetype="audio/wav", as_attachment=False)
    response.headers["Content-Length"] = str(file_size)
    response.headers["Connection"] = "keep-alive"
    response.headers["X-Response-Time"] = f"{total_time:.2f}"
    response.headers["X-STT-Time"] = f"{stt_time:.2f}"
    response.headers["X-GPT-Time"] = f"{gpt_time:.2f}"
    response.headers["X-TTS-Time"] = f"{tts_time:.2f}"
    return response

@app.route("/wakeup", methods=["GET"])
def wakeup():
    try:
        # Look for WAV files in the audio directory
        wav_files = [f for f in os.listdir(AUDIO_DIR) if f.endswith('.wav')]
        
        if not wav_files:
            return jsonify({"error": "No audio files available"}), 404
        
        # Return the most recent WAV file
        wav_files.sort(key=lambda x: os.path.getmtime(os.path.join(AUDIO_DIR, x)), reverse=True)
        latest_file = wav_files[0]
        file_path = os.path.join(AUDIO_DIR, latest_file)
        
        # Verify file exists and has content
        if not os.path.exists(file_path) or os.path.getsize(file_path) == 0:
            return jsonify({"error": "Audio file not found or empty"}), 404
        
        print(f"[INFO] Serving wakeup audio: {latest_file}")
        
        # Send the WAV file with proper headers
        response = send_file(file_path, mimetype="audio/wav", as_attachment=False)
        response.headers["Content-Length"] = str(os.path.getsize(file_path))
        response.headers["Connection"] = "keep-alive"
        return response
        
    except Exception as e:
        print(f"[ERROR] Wakeup route error: {str(e)}")
        return jsonify({"error": "Internal server error"}), 500

@app.route("/text_upload", methods=["POST"])
def upload_text():
    # Start timing measurements  
    start_time = time.time()
    timing_log = {"start": start_time}
    
    # New route for receiving text directly from ESP32 with local STT
    try:
        data = request.get_json()
        if not data or 'text' not in data:
            return jsonify({"error": "Expected JSON with 'text' field"}), 400
        
        user_text = data['text']
        session_id = data.get('session_id', 'esp32_default')  # ESP32 can send session ID
        print(f"[INFO] Received text from local STT: {user_text} (Session: {session_id})")
    except Exception as e:
        return jsonify({"error": f"Invalid JSON: {str(e)}"}), 400

    timing_log["text_received"] = time.time()
    
    # Process with GPT with memory context (skip STT since we have text)
    gpt_start = time.time()
    gpt_reply = get_gpt_reply(user_text, session_id)
    timing_log["gpt_complete"] = time.time()
    if not gpt_reply:
        return jsonify({"error": "GPT processing failed"}), 500
    print("[GPT]", gpt_reply)

    # Generate TTS response
    tts_start = time.time()
    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    output_path = os.path.join(TEMP_DIR, f"reply_text_{timestamp}.wav")
    synthesize_speech(gpt_reply, output_path)

    # Wait for TTS output file
    max_retries = 60
    retry_delay = 1.0
    for attempt in range(max_retries):
        if os.path.exists(output_path) and os.path.getsize(output_path) > 0:
            break
        print(f"[DEBUG] Attempt {attempt + 1}: File not ready, waiting {retry_delay}s...")
        time.sleep(retry_delay)
    else:
        return jsonify({"error": "Speech synthesis failed"}), 500

    timing_log["tts_complete"] = time.time()
    file_size = os.path.getsize(output_path)
    
    # Calculate timing breakdown (Local STT version)
    total_time = timing_log["tts_complete"] - timing_log["start"]
    gpt_time = timing_log["gpt_complete"] - timing_log["text_received"]
    tts_time = timing_log["tts_complete"] - timing_log["gpt_complete"]
    
    print(f"\n=== LOCAL STT RESPONSE TIME ANALYSIS ===")
    print(f"Total Response Time: {total_time:.2f}s")
    print(f"├─ GPT Generation: {gpt_time:.2f}s ({gpt_time/total_time*100:.1f}%)")
    print(f"└─ TTS Generation: {tts_time:.2f}s ({tts_time/total_time*100:.1f}%)")
    print(f"Audio file size: {file_size} bytes")
    print(f"NOTE: STT was done locally on ESP32 (not measured here)")
    print(f"=== END TIMING ANALYSIS ===\n")
    
    response = send_file(output_path, mimetype="audio/wav", as_attachment=False)
    response.headers["Content-Length"] = str(file_size)
    response.headers["Connection"] = "keep-alive"
    response.headers["X-Response-Time"] = f"{total_time:.2f}"
    response.headers["X-GPT-Time"] = f"{gpt_time:.2f}"
    response.headers["X-TTS-Time"] = f"{tts_time:.2f}"
    return response

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=int(os.getenv("PORT", 5005)), debug=True)
# To run this app, ensure you have the required environment variables set:
# - OPENAI_API_KEY for OpenAI API access
# - PORT for the Flask server port (default is 5005)
# Make sure to install the required packages:
# pip install Flask openai
# Also, ensure you have the whisper_stt.py and gpt_reply.py modules implemented as needed.
# You can run the app with: python app.py
# The app will listen for audio uploads at the /upload endpoint and respond with synthesized speech.    
