#include "battery.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "BATTERY";
static adc_oneshot_unit_handle_t s_adc1_handle = NULL;
static adc_cali_handle_t s_cali_handle = NULL;
static bool s_calibrated = false;
static bool s_initialized = false;

/* Li-Ion 1S Discharge Curve Table (mV -> Percentage) */
static const struct {
    uint16_t mv;
    uint8_t  pct;
} s_discharge_curve[] = {
    { 4200, 100 },
    { 4050,  90 },
    { 3950,  80 },
    { 3850,  70 },
    { 3780,  60 },
    { 3730,  50 },
    { 3690,  40 },
    { 3650,  30 },
    { 3600,  20 },
    { 3500,  10 },
    { 3300,   0 }
};

esp_err_t battery_init(void)
{
    if (s_initialized) return ESP_OK;

    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&init_config, &s_adc1_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init ADC1 oneshot unit: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chan_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_oneshot_config_channel(s_adc1_handle, ADC_CHANNEL_6, &chan_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to config ADC1 CH6 (GPIO 34): %s", esp_err_to_name(err));
        return err;
    }

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_config, &s_cali_handle) == ESP_OK) {
        s_calibrated = true;
        ESP_LOGI(TAG, "ADC1 calibration: Line fitting scheme enabled");
    }
#endif

    s_initialized = true;
    ESP_LOGI(TAG, "Battery monitor initialized on GPIO 34 (ADC1_CH6)");
    return ESP_OK;
}

uint32_t battery_get_millivolts(void)
{
    if (!s_initialized) battery_init();
    if (!s_adc1_handle) return 0;

    /* 16-sample multisampling for high noise rejection */
    int raw_sum = 0;
    for (int i = 0; i < 16; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc1_handle, ADC_CHANNEL_6, &raw) == ESP_OK) {
            raw_sum += raw;
        }
    }
    int raw_avg = raw_sum / 16;

    int voltage_mv = 0;
    if (s_calibrated && s_cali_handle) {
        adc_cali_raw_to_voltage(s_cali_handle, raw_avg, &voltage_mv);
    } else {
        /* Approximate 12-bit conversion at 12dB atten (~2450mV full-scale) */
        voltage_mv = (raw_avg * 2450) / 4095;
    }

    /* Apply 2:1 divider ratio (100k + 100k) */
    uint32_t batt_mv = (uint32_t)(voltage_mv * BATTERY_DIVIDER_RATIO);
    return batt_mv;
}

uint8_t battery_get_percentage(void)
{
    uint32_t mv = battery_get_millivolts();
    size_t n = sizeof(s_discharge_curve) / sizeof(s_discharge_curve[0]);

    if (mv >= s_discharge_curve[0].mv) return 100;
    if (mv <= s_discharge_curve[n - 1].mv) return 0;

    for (size_t i = 0; i < n - 1; i++) {
        if (mv <= s_discharge_curve[i].mv && mv >= s_discharge_curve[i + 1].mv) {
            uint32_t v_high = s_discharge_curve[i].mv;
            uint32_t v_low  = s_discharge_curve[i + 1].mv;
            uint8_t  p_high = s_discharge_curve[i].pct;
            uint8_t  p_low  = s_discharge_curve[i + 1].pct;

            /* Linear interpolation */
            uint32_t pct = p_low + ((mv - v_low) * (p_high - p_low)) / (v_high - v_low);
            return (uint8_t)pct;
        }
    }
    return 50;
}
