#include "mbed.h"
#include "rtos.h"
#include "arm_math.h"
#include "GYRO_DISCO_F429ZI.h"
#include "LCD_DISCO_F429ZI.h"
#include <queue>
#include <list>
#include <cmath>

#define SAMPLING_FREQ 40 // Hz
#define FFT_SIZE 256
#define FFT_NUM_BINS (FFT_SIZE / 2)
#define TREMOR_THRESHOLD 2
#define TREMOR_FREQ_MIN 3 // min freq of tremor
#define TREMOR_FREQ_MAX 6 // max freq of tremor

#define SCALING_FACTOR (17.5f * 0.0174532925199432957692236907684886f / 1000.0f)
#define CTRL_REG1 0x20
#define CTRL_REG1_CONFIG 0b01111111
#define CTRL_REG4 0x23
#define CTRL_REG4_CONFIG 0b00001000
#define SPI_FLAG 1
#define OUT_X_L 0x28

// SPI related var
EventFlags flags;
SPI spi(PF_9, PF_8, PF_7, PC_1, use_gpio_ssel);
uint8_t write_buf[8], read_buf[8];

// FFT var
arm_rfft_fast_instance_f32 fft_inst;
float32_t fft_in[FFT_SIZE];
float32_t fft_out[FFT_SIZE];
float32_t fft_magnitude[FFT_NUM_BINS];

// Band-pass filter parameters
#define LPF_ALPHA 0.1f    // low-pass  coef
#define HPF_ALPHA 0.1f    // high-pass coef
float32_t lpf_out = 0.0f; // low-pass
float32_t hpf_out = 0.0f; // high-pass

// Func: peak prominence
float peakProminence(float *spectrum, int peakIdx, int halfWindowSize)
{
    float peak = spectrum[peakIdx];
    float leftMax = 0.0f;
    float rightMax = 0.0f;

    // max magnitude to the left peak
    for (int i = std::max(0, peakIdx - halfWindowSize); i < peakIdx; i++)
    {
        leftMax = std::max(leftMax, spectrum[i]);
    }

    // max magnitude to the right peak
    for (int i = peakIdx + 1; i < std::min(FFT_NUM_BINS, peakIdx + halfWindowSize); i++)
    {
        rightMax = std::max(rightMax, spectrum[i]);
    }

    float peakPro = peak - std::max(leftMax, rightMax);
    return peakPro;
}

void spi_cb(int event)
{
    flags.set(SPI_FLAG);
}

LCD_DISCO_F429ZI lcd;

void LCD_Setup()
{
    uint8_t test[20];
    BSP_LCD_SetFont(&Font20);
    lcd.Clear(LCD_COLOR_BLUE);
    lcd.SetBackColor(LCD_COLOR_BLUE);
    lcd.SetTextColor(LCD_COLOR_YELLOW);
    lcd.DisplayStringAt(0, LINE(3), (uint8_t *)"Parkinson", CENTER_MODE);
    lcd.DisplayStringAt(0, LINE(5), (uint8_t *)"Detection", CENTER_MODE);
    lcd.SetTextColor(LCD_COLOR_WHITE);
}

int main()
{
    LCD_Setup();

    spi.format(8, 3);
    spi.frequency(1'000'000);

    write_buf[0] = CTRL_REG1;
    write_buf[1] = CTRL_REG1_CONFIG;
    spi.transfer(write_buf, 2, read_buf, 2, spi_cb);
    flags.wait_all(SPI_FLAG);

    write_buf[0] = CTRL_REG4;
    write_buf[1] = CTRL_REG4_CONFIG;
    spi.transfer(write_buf, 2, read_buf, 2, spi_cb);
    flags.wait_all(SPI_FLAG);

    arm_rfft_fast_init_f32(&fft_inst, FFT_SIZE);

    while (1)
    {
        for (int i = 0; i < FFT_SIZE; i++)
        {
            write_buf[0] = OUT_X_L | 0x80 | 0x40;
            spi.transfer(write_buf, 7, read_buf, 7, spi_cb);
            flags.wait_all(SPI_FLAG);

            uint16_t raw_gx = (((uint16_t)read_buf[2]) << 8) | ((uint16_t)read_buf[1]);
            uint16_t raw_gy = (((uint16_t)read_buf[4]) << 8) | ((uint16_t)read_buf[3]);
            uint16_t raw_gz = (((uint16_t)read_buf[6]) << 8) | ((uint16_t)read_buf[5]);

            // using scaling to convert
            float gx = ((float)raw_gx) * SCALING_FACTOR;
            float gy = ((float)raw_gy) * SCALING_FACTOR;
            float gz = ((float)raw_gz) * SCALING_FACTOR;

            // use low-pass
            lpf_out = lpf_out + LPF_ALPHA * (gx + gy + gz - lpf_out);
            // high-pass to remove DC offset
            hpf_out = HPF_ALPHA * (hpf_out + lpf_out - hpf_out);
            fft_in[i] = hpf_out;
            ThisThread::sleep_for(1000 / SAMPLING_FREQ);
        }

        arm_rfft_fast_f32(&fft_inst, fft_in, fft_out, 0);

        // find peak freq
        float max_magnitude = 0.0f;
        float peak_freq = 0.0f;
        int peak_idx = -1;
        for (int i = 0; i < FFT_NUM_BINS; i++)
        {
            float freq = (float)i * SAMPLING_FREQ / FFT_SIZE;
            if (freq >= TREMOR_FREQ_MIN && freq <= TREMOR_FREQ_MAX)
            {
                if (fft_out[i] > max_magnitude)
                {
                    max_magnitude = fft_out[i];
                    peak_freq = freq;
                    peak_idx = i;
                }
            }
        }

        // see if peak freq in tremor range and prominent
        if (peak_idx != -1)
        {
            float peak_prominence = peakProminence(fft_out, peak_idx, 5); // Adjust the half window size as needed
            if (peak_freq >= TREMOR_FREQ_MIN && peak_freq <= TREMOR_FREQ_MAX && peak_prominence > TREMOR_THRESHOLD)
            {
                lcd.ClearStringLine(7);
                lcd.DisplayStringAt(0, LINE(7), (uint8_t *)"Parkinson: Yes", LEFT_MODE);
                lcd.ClearStringLine(9);
                char freq_str[20];
                sprintf(freq_str, "Frequency: %.2fHz", peak_freq);
                lcd.DisplayStringAt(0, LINE(9), (uint8_t *)freq_str, LEFT_MODE);
                lcd.ClearStringLine(11);
                char intensity_str[50];
                sprintf(intensity_str, "Intensity: %.2f", max_magnitude);
                lcd.DisplayStringAt(0, LINE(11), (uint8_t *)intensity_str, LEFT_MODE);
                lcd.DisplayStringAt(0, LINE(13), (uint8_t *)"Level: ", LEFT_MODE);
                lcd.DisplayStringAt(60, LINE(13), (uint8_t *)((max_magnitude < 15) ? "Low" : ((max_magnitude < 25 and max_magnitude >= 15) ? "Medium" : "High")), LEFT_MODE);
            }
            else
            {
                lcd.ClearStringLine(7);
                lcd.DisplayStringAt(0, LINE(7), (uint8_t *)"Parkinson: No", LEFT_MODE);
                lcd.ClearStringLine(9);
                lcd.ClearStringLine(11);
                lcd.ClearStringLine(13);
            }
        }
        else
        {
            // no peak in the tremor range
            lcd.ClearStringLine(7);
            lcd.ClearStringLine(9);
            lcd.ClearStringLine(11);
            lcd.ClearStringLine(13);
        }
    }
}
