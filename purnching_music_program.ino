#include <Wire.h>
#include <SDHCI.h>
#include <Audio.h>

// 定数定義
namespace Constants {
    // ADXL345の設定
    constexpr uint8_t ADXL345_ADDRESS = 0x53;
    constexpr uint8_t ADXL345_REG_POWER_CTL = 0x2D;
    constexpr uint8_t ADXL345_REG_DATA_FORMAT = 0x31;
    constexpr uint8_t ADXL345_REG_DATAX0 = 0x32;
    constexpr uint8_t ADXL345_REG_DATAY0 = 0x34;
    constexpr uint8_t ADXL345_REG_DATAZ0 = 0x36;
    constexpr uint8_t RANGE_8G = 0x02;
    constexpr uint8_t RANGE_2G = 0x00; // ±2g 範囲設定


    // ジャンプ検出設定
    constexpr int16_t JUMP_THRESHOLD = 80;
    constexpr int16_t MIN_ACCELERATION = 80;  // C3 を 100 にマッピング
    constexpr int16_t MAX_ACCELERATION = 400;
    constexpr unsigned long JUMP_COOLDOWN = 500;
    
    // オーディオバッファサイズ
    constexpr size_t BUFFER_SIZE = 1024;
    
    // 音階ファイル設定
    constexpr int SCALE_SIZE = 8;
    const char* SCALE_FILES[SCALE_SIZE] = {
        "C3.wav", "D3.wav", "E3.wav", "F3.wav", 
        "G3.wav", "A3.wav", "B3.wav", "C4.wav"
    };
}

// クラス定義：加速度センサー管理
class AccelerometerManager {
private:
    int16_t peak_acceleration = 0;
    bool is_jumping = false;  // ジャンプ中フラグ
    unsigned long last_jump_time = 0;

public:
    void initialize() {
        Wire.begin();
        writeRegister(Constants::ADXL345_REG_DATA_FORMAT, Constants::RANGE_2G);
        writeRegister(Constants::ADXL345_REG_POWER_CTL, 0x08);
    }

    bool detectJump() {
        int16_t z = readAxis(Constants::ADXL345_REG_DATAZ0);
        z = abs(z);
        unsigned long current_time = millis();

        // ジャンプ開始の検出：加速度が急激に増加したとき
        if (z > Constants::JUMP_THRESHOLD && !is_jumping && (current_time - last_jump_time > Constants::JUMP_COOLDOWN)) {
            is_jumping = true;
            peak_acceleration = z;  // ジャンプピークの記録
            last_jump_time = current_time;
            return true;
        }

        // 着地判定：加速度が閾値以下になるとジャンプ終了と見なす
        if (z < Constants::JUMP_THRESHOLD && is_jumping) {
            is_jumping = false;  // ジャンプ終了
            peak_acceleration = 0;  // ピーク加速度のリセット
        }

        // 現在の加速度がジャンプ中における最大値を更新する
        if (is_jumping && z > peak_acceleration) {
            peak_acceleration = z;
        }

        return false;
    }

    int16_t getPeakAcceleration() const {
        return peak_acceleration;
    }

    void resetPeakAcceleration() {
        peak_acceleration = 0;
    }

private:
    void writeRegister(uint8_t reg, uint8_t value) {
        Wire.beginTransmission(Constants::ADXL345_ADDRESS);
        Wire.write(reg);
        Wire.write(value);
        Wire.endTransmission();
    }

    int16_t readAxis(uint8_t axis_reg) {
        Wire.beginTransmission(Constants::ADXL345_ADDRESS);
        Wire.write(axis_reg);
        Wire.endTransmission(false);
        
        Wire.requestFrom(Constants::ADXL345_ADDRESS, 2);
        return Wire.read() | (Wire.read() << 8);
    }
};

// クラス定義：オーディオ管理
class AudioManager {
private:
    SDClass& sd;
    AudioClass* audio;
    uint8_t buffer[Constants::BUFFER_SIZE];
    WavContainerFormatParser parser;

public:
    AudioManager(SDClass& sd_instance) : sd(sd_instance) {
        audio = AudioClass::getInstance();
    }

    bool initialize() {
        if (!sd.begin()) {
            Serial.println("SD card initialization failed");
            return false;
        }

        audio->begin();
        audio->setRenderingClockMode(AS_CLKMODE_NORMAL);
        audio->setPlayerMode(AS_SETPLAYER_OUTPUTDEVICE_SPHP, AS_SP_DRV_MODE_LINEOUT);

        err_t err = audio->initPlayer(AudioClass::Player0, 
                                      AS_CODECTYPE_WAV,
                                      "/mnt/sd0/BIN",
                                      48000,
                                      16,
                                      2);

        if (err != AUDIOLIB_ECODE_OK) {
            Serial.println("Audio player initialization failed");
            return false;
        }

        audio->setVolume(-160);
        return true;
    }

    void resetAudioPlayer() {
        // 音声プレイヤーを停止し、再初期化
        audio->stopPlayer(AudioClass::Player0);
        audio->startPlayer(AudioClass::Player0);
    }

    void playSound(int scale_index) {
        resetAudioPlayer();  // プレイヤーの状態をリセット
        if (scale_index < 0 || scale_index >= Constants::SCALE_SIZE) {
            Serial.println("Invalid scale index");
            return;
        }

        char filepath[32];
        snprintf(filepath, sizeof(filepath), "/mnt/sd0/%s", 
                Constants::SCALE_FILES[scale_index]);

        fmt_chunk_t fmt;
        handel_wav_parser_t* handle = 
            (handel_wav_parser_t*)parser.parseChunk(filepath, &fmt);

        if (!handle) {
            Serial.print("WAV parser error for file: ");
            Serial.println(filepath);
            return;
        }

        File file = sd.open(Constants::SCALE_FILES[scale_index]);
        if (!file) {
            Serial.println("File open error");
            return;
        }

        playWavFile(file, handle);  // ここで playWavFile メソッドを呼び出します
        file.close();
    }

private:
    // playWavFile メソッドをクラス内部に追加します
    void playWavFile(File& file, handel_wav_parser_t* handle) {
        file.seek(handle->data_offset);
        uint32_t remain_size = handle->data_size;

        audio->stopPlayer(AudioClass::Player0);

        while (remain_size > 0) {
            size_t supply_size = file.read(buffer, sizeof(buffer));
            if (supply_size == 0) {
                // ファイル読み込みエラー
                Serial.println("Error reading file");
                break;
            }

            remain_size -= supply_size;
            err_t err = audio->writeFrames(AudioClass::Player0, buffer, supply_size);
            if (err != AUDIOLIB_ECODE_OK) {
                // オーディオ書き込みエラー
                Serial.print("Error writing frames: ");
                Serial.println(err);
                break;
            }
        }

        audio->startPlayer(AudioClass::Player0);
        file.close();  // ファイルのクローズ
    }
};

// メインクラス
class JumpSoundSystem {
private:
    AccelerometerManager accelerometer;
    AudioManager audio;

public:
    JumpSoundSystem(SDClass& sd) : audio(sd) {}

    void initialize() {
        Serial.begin(115200);
        
        accelerometer.initialize();
        
        if (audio.initialize()) {
            //Serial.println("Audio system initialized");
        }
    }

    void update() {
        unsigned long current_time = millis();  // 現在の時間を取得

        if (accelerometer.detectJump()) {
            int scale_index = calculateScaleIndex(accelerometer.getPeakAcceleration());
            audio.playSound(scale_index);
            
            Serial.print("Peak Acceleration: ");
            Serial.println(accelerometer.getPeakAcceleration());
            
            accelerometer.resetPeakAcceleration();
        }
        delay(100);  // 100msの遅延を入れて、次の検出まで少し待機
    }

private:
    int calculateScaleIndex(int16_t acceleration) {
        // 加速度を音階の範囲にマッピング
        acceleration = constrain(acceleration, 
                                 Constants::MIN_ACCELERATION,
                                 Constants::MAX_ACCELERATION);

        // 加速度が100の場合はC3にマッピング
        float mapped = map(acceleration,
                           Constants::MIN_ACCELERATION,
                           Constants::MAX_ACCELERATION,
                           0,
                           Constants::SCALE_SIZE - 1);

        // 音階インデックスを丸めて返す
        return (int)round(mapped);
    }
};

// グローバルインスタンス
SDClass theSD;
JumpSoundSystem jumpSystem(theSD);

void setup() {
    jumpSystem.initialize();
}

void loop() {
    jumpSystem.update();
}
