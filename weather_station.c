/*
 * Projeto: Estação Meteorológica BitDogLab
 * Plataforma: Raspberry Pi Pico W (BitDogLab)
 * Autor: Elmer Carvalho
 * Descrição: Sistema embarcado para leitura dos sensores AHT20 (temperatura/umidade) e BMP280 (pressão/temperatura),
 *            exibição local em display OLED SSD1306, servidor web responsivo com AJAX, alertas visuais/sonoros e
 *            configuração de limites/offsets via interface web e botões físicos.
 * Versão: Formulário com 5 containers, lógica de POST /cfg revisada para atualizações individuais robustas.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "lib/ssd1306.h"
#include "lib/aht20.h"
#include "lib/bmp280.h"
#include "pico/bootrom.h"

// ===================== DEFINIÇÕES DE HARDWARE =====================
#define I2C_PORT_SENSORES i2c0
#define I2C_SENS_SDA 0
#define I2C_SENS_SCL 1

#define I2C_PORT_DISPLAY i2c1
#define I2C_DISP_SDA 14
#define I2C_DISP_SCL 15
#define DISPLAY_ADDRESS 0x3C

#define LED_RED_PIN 13
#define LED_GREEN_PIN 11
#define LED_BLUE_PIN 12
#define BUZZER_PIN 21
#define PWM_DIVISOR 50
#define PWM_WRAP_VALUE 4000

#define BTN_1 5  // Navegação/Menu
#define BTN_2 6  // Seleção/Configuração
#define BTN_3 22 // Modo especial/Reset

// ===================== DEFINIÇÕES DE SISTEMA =====================
#define SENSOR_READ_INTERVAL_MS 2000
#define ALERT_BEEP_DURATION 200
#define ALERT_BEEP_PAUSE 100
#define WIFI_SSID "Minha Internet"
#define WIFI_PASS "minhasenha157"
#define TCP_TIMEOUT_MS 10000
#define TCP_CHUNK_SIZE 512
#define MAX_REQUEST_SIZE 1024
#define WIFI_RECONNECT_INTERVAL_MS 5000

// Limites padrão saudáveis para humanos
#define TEMP_MIN_DEFAULT 15.0f
#define TEMP_MAX_DEFAULT 30.0f
#define HUM_MIN_DEFAULT 30.0f
#define HUM_MAX_DEFAULT 70.0f
#define PRESS_MIN_DEFAULT 950.0f
#define PRESS_MAX_DEFAULT 1050.0f

// ===================== ESTRUTURAS DE DADOS =====================
typedef struct
{
    float temp_aht20;
    float hum_aht20;
    float press_bmp280;
} sensor_data_t;

typedef struct
{
    float temp_min, temp_max;
    float hum_min, hum_max;
    float press_min, press_max;
    float temp_offset, hum_offset, press_offset;
} config_limits_t;

typedef struct
{
    struct tcp_pcb *pcb;
    absolute_time_t timeout;
    bool response_sent;
    const char *remaining_data;
    size_t remaining_len;
} conn_state_t;

// ===================== VARIÁVEIS GLOBAIS =====================
ssd1306_t display;
sensor_data_t sensor_data;
config_limits_t config = {
    .temp_min = TEMP_MIN_DEFAULT, .temp_max = TEMP_MAX_DEFAULT,
    .hum_min = HUM_MIN_DEFAULT, .hum_max = HUM_MAX_DEFAULT,
    .press_min = PRESS_MIN_DEFAULT, .press_max = PRESS_MAX_DEFAULT,
    .temp_offset = 0, .hum_offset = 0, .press_offset = 0
};
SemaphoreHandle_t mutex_sensor;
SemaphoreHandle_t mutex_config;
volatile bool alert_active = false;
volatile bool wifi_connected = false;
volatile bool log_medicoes = true;

#define MAX_CONNECTIONS 4
conn_state_t *active_connections[MAX_CONNECTIONS] = {NULL};

// ===================== PROTÓTIPOS =====================
void inicializar_hardware(void);
void inicializar_display(void);
void inicializar_leds(void);
void inicializar_buzzer(void);
void inicializar_botoes(void);
void atualizar_display(void);
void emitir_alerta(void);
void atualizar_led_status(void);
void tarefa_leitura_sensores(void *param);
void tarefa_webserver(void *param);
void tarefa_alerta(void *param);
void tarefa_display(void *param);
void tarefa_timeout(void *param);
void manipulador_interrupcao_gpio(uint gpio, uint32_t eventos);
void tratar_botao(uint btn);
static err_t webserver_sent(void *arg, struct tcp_pcb *tpcb, u16_t len);
static err_t webserver_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static err_t webserver_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
void send_http_response(struct tcp_pcb *tpcb, const char *header, const char *body, conn_state_t *state);
void close_connection(conn_state_t *state);

// ===================== HTML/CSS/JS EMBUTIDO =====================
const char *html_page =
    "<!DOCTYPE html><html lang='pt'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>Estação BitDogLab</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;margin:0;padding:20px;background:#222;color:#eee;display:flex;flex-direction:column;align-items:center;min-height:100vh}"
    "h1{font-size:1.8em;margin-bottom:20px;text-align:center}"
    "#dados{font-size:1.2em;margin:20px 0;padding:10px;background:#333;border-radius:5px;width:100%;max-width:400px;text-align:center}"
    ".graficos{display:flex;justify-content:center;gap:20px;flex-wrap:wrap}"
    ".grafico-container{width:300px;margin:10px 0}"
    ".grafico-container canvas{border:1px solid #555}"
    ".grafico-container h3{font-size:1.2em;margin:5px 0;color:#4CAF50;text-align:center}"
    ".grafico-container .legend{font-size:0.9em;color:#bbb;text-align:center}"
    "#cfg{width:100%;max-width:600px;background:#333;padding:20px;border-radius:5px;display:grid;gap:15px}"
    ".title-container{text-align:center}"
    ".pair-container{display:grid;grid-template-columns:1fr 1fr;gap:10px;align-items:center}"
    ".offset-container{display:grid;grid-template-columns:150px 100px 100px;gap:10px;align-items:center}"
    ".button-container{display:grid;grid-template-columns:1fr;justify-items:center}"
    ".status-container{text-align:center;font-size:1em;color:#4CAF50}"
    ".pair-container label,.offset-container label{font-size:1em;color:#eee;text-align:right;min-width:100px}"
    "input[type=number]{width:100px;padding:5px;border:1px solid #555;border-radius:3px;background:#444;color:#eee;box-sizing:border-box}"
    ".current-value{font-size:0.9em;color:#4CAF50;text-align:right;width:100px}"
    "button{padding:8px 16px;background:#4CAF50;border:none;border-radius:3px;color:white;cursor:pointer}"
    "button:hover{background:#45a049}"
    "@media(max-width:900px){.graficos{flex-direction:column;align-items:center}.grafico-container{width:100%;max-width:300px}}"
    "@media(max-width:600px){body{padding:10px}#dados,#cfg{max-width:100%}.pair-container{grid-template-columns:1fr}.offset-container{grid-template-columns:120px 80px 80px}input[type=number]{width:80px}.pair-container label,.offset-container label{min-width:120px}}"
    "</style></head>"
    "<body><h1>Estação Meteorológica</h1><div id='dados'>Carregando...</div>"
    "<div class='graficos'>"
    "<div class='grafico-container'><h3>Temperatura (°C)</h3><canvas id='grafico-temp' width='300' height='100'></canvas><div id='legend-temp' class='legend'></div></div>"
    "<div class='grafico-container'><h3>Umidade (%)</h3><canvas id='grafico-hum' width='300' height='100'></canvas><div id='legend-hum' class='legend'></div></div>"
    "<div class='grafico-container'><h3>Pressão (hPa)</h3><canvas id='grafico-press' width='300' height='100'></canvas><div id='legend-press' class='legend'></div></div>"
    "</div>"
    "<form id='cfg'>"
    "<div class='title-container'><h2>Configuração</h2></div>"
    "<div class='pair-container'>"
    "<div><label>Temp Mín (°C):</label><input name='temp_min' type='number' step='0.1' placeholder='15.0'><span class='current-value' id='current-temp-min'></span></div>"
    "<div><label>Temp Máx (°C):</label><input name='temp_max' type='number' step='0.1' placeholder='30.0'><span class='current-value' id='current-temp-max'></span></div>"
    "</div>"
    "<div class='pair-container'>"
    "<div><label>Umid Mín (%):</label><input name='hum_min' type='number' step='0.1' placeholder='30.0'><span class='current-value' id='current-hum-min'></span></div>"
    "<div><label>Umid Máx (%):</label><input name='hum_max' type='number' step='0.1' placeholder='70.0'><span class='current-value' id='current-hum-max'></span></div>"
    "</div>"
    "<div class='pair-container'>"
    "<div><label>Press Mín (hPa):</label><input name='press_min' type='number' step='0.1' placeholder='950.0'><span class='current-value' id='current-press-min'></span></div>"
    "<div><label>Press Máx (hPa):</label><input name='press_max' type='number' step='0.1' placeholder='1050.0'><span class='current-value' id='current-press-max'></span></div>"
    "</div>"
    "<div class='offset-container'><h3>Offsets</h3></div>"
    "<div class='offset-container'><label>Offset Temp (°C):</label><input name='temp_offset' type='number' step='0.1' placeholder='0.0'><span class='current-value' id='current-temp-offset'></span></div>"
    "<div class='offset-container'><label>Offset Umid (%):</label><input name='hum_offset' type='number' step='0.1' placeholder='0.0'><span class='current-value' id='current-hum-offset'></span></div>"
    "<div class='offset-container'><label>Offset Press (hPa):</label><input name='press_offset' type='number' step='0.1' placeholder='0.0'><span class='current-value' id='current-press-offset'></span></div>"
    "<div class='button-container'><button type='submit'>Salvar</button></div>"
    "<div class='status-container' id='status'></div>"
    "</form>"
    "<script>"
    "let d = []; const dadosEl = document.getElementById('dados'); const statusEl = document.getElementById('status');"
    "let config = {temp_min: 15, temp_max: 30, hum_min: 30, hum_max: 70, press_min: 950, press_max: 1050, temp_offset: 0, hum_offset: 0, press_offset: 0};"
    "async function loadConfig() {"
    "  try {"
    "    const r = await fetch('/config', { method: 'GET', headers: { 'Accept': 'application/json' } });"
    "    if (!r.ok) throw new Error(`Erro HTTP ${r.status}: ${r.statusText}`);"
    "    config = await r.json();"
    "    document.getElementById('current-temp-min').textContent = `${config.temp_min.toFixed(1)}`;"
    "    document.getElementById('current-temp-max').textContent = `${config.temp_max.toFixed(1)}`;"
    "    document.getElementById('current-hum-min').textContent = `${config.hum_min.toFixed(1)}`;"
    "    document.getElementById('current-hum-max').textContent = `${config.hum_max.toFixed(1)}`;"
    "    document.getElementById('current-press-min').textContent = `${config.press_min.toFixed(1)}`;"
    "    document.getElementById('current-press-max').textContent = `${config.press_max.toFixed(1)}`;"
    "    document.getElementById('current-temp-offset').textContent = `${config.temp_offset.toFixed(1)}`;"
    "    document.getElementById('current-hum-offset').textContent = `${config.hum_offset.toFixed(1)}`;"
    "    document.getElementById('current-press-offset').textContent = `${config.press_offset.toFixed(1)}`;"
    "    document.querySelector('input[name=\"temp_min\"]').value = config.temp_min.toFixed(1);"
    "    document.querySelector('input[name=\"temp_max\"]').value = config.temp_max.toFixed(1);"
    "    document.querySelector('input[name=\"hum_min\"]').value = config.hum_min.toFixed(1);"
    "    document.querySelector('input[name=\"hum_max\"]').value = config.hum_max.toFixed(1);"
    "    document.querySelector('input[name=\"press_min\"]').value = config.press_min.toFixed(1);"
    "    document.querySelector('input[name=\"press_max\"]').value = config.press_max.toFixed(1);"
    "    document.querySelector('input[name=\"temp_offset\"]').value = config.temp_offset.toFixed(1);"
    "    document.querySelector('input[name=\"hum_offset\"]').value = config.hum_offset.toFixed(1);"
    "    document.querySelector('input[name=\"press_offset\"]').value = config.press_offset.toFixed(1);"
    "  } catch (e) {"
    "    console.error('Erro ao carregar configuração:', e);"
    "    statusEl.textContent = `Erro ao carregar config: ${e.message}`; statusEl.style.color = '#f44336';"
    "  }"
    "}"
    "async function atualiza() {"
    "  try {"
    "    const r = await fetch('/json', { method: 'GET', headers: { 'Accept': 'application/json' } });"
    "    if (!r.ok) throw new Error(`Erro HTTP ${r.status}: ${r.statusText}`);"
    "    const j = await r.json();"
    "    dadosEl.textContent = `Temp: ${j.temp_aht20.toFixed(1)}°C | Umid: ${j.hum_aht20.toFixed(1)}% | Press: ${j.press_bmp280.toFixed(1)}hPa`;"
    "    d.push(j); if (d.length > 50) d.shift();"
    "    const drawGraph = (canvasId, dataKey, color, min, max, unit) => {"
    "      const canvas = document.getElementById(canvasId);"
    "      const ctx = canvas.getContext('2d');"
    "      ctx.clearRect(0, 0, canvas.width, canvas.height);"
    "      const range = max - min; const scale = 80 / range;"
    "      ctx.strokeStyle = '#555'; ctx.lineWidth = 1;"
    "      ctx.beginPath(); ctx.moveTo(0, 10); ctx.lineTo(300, 10); ctx.stroke();"
    "      ctx.beginPath(); ctx.moveTo(0, 90); ctx.lineTo(300, 90); ctx.stroke();"
    "      ctx.font = '10px Arial'; ctx.fillStyle = '#bbb';"
    "      ctx.fillText(`${max.toFixed(1)}${unit}`, 5, 15);"
    "      ctx.fillText(`${min.toFixed(1)}${unit}`, 5, 95);"
    "      ctx.strokeStyle = color; ctx.lineWidth = 2; ctx.beginPath();"
    "      for (let i = 0; i < d.length; i++) {"
    "        const y = 90 - ((d[i][dataKey] - min) * scale);"
    "        ctx.lineTo(i * 6, y);"
    "      }"
    "      ctx.stroke();"
    "      document.getElementById(`legend-${canvasId.split('-')[1]}`).textContent = `Atual: ${j[dataKey].toFixed(1)}${unit} | Min: ${min.toFixed(1)}${unit} | Max: ${max.toFixed(1)}${unit}`;"
    "    };"
    "    drawGraph('grafico-temp', 'temp_aht20', '#ff5555', config.temp_min, config.temp_max, '°C');"
    "    drawGraph('grafico-hum', 'hum_aht20', '#55aaff', config.hum_min, config.hum_max, '%');"
    "    drawGraph('grafico-press', 'press_bmp280', '#55ff55', config.press_min, config.press_max, 'hPa');"
    "  } catch (e) {"
    "    console.error('Erro ao atualizar dados:', e);"
    "    dadosEl.textContent = 'Erro ao carregar dados';"
    "    statusEl.textContent = `Erro: ${e.message}`; statusEl.style.color = '#f44336';"
    "  }"
    "}"
    "setInterval(atualiza, 2000); atualiza(); loadConfig();"
    "document.getElementById('cfg').addEventListener('submit', async e => {"
    "  e.preventDefault(); statusEl.textContent = 'Salvando...'; statusEl.style.color = '#4CAF50';"
    "  try {"
    "    const f = new FormData(e.target);"
    "    const data = new URLSearchParams();"
    "    for (let [key, value] of f.entries()) {"
    "      if (value.trim() !== '') data.append(key, value);"
    "    }"
    "    if (data.toString() === '') {"
    "      statusEl.textContent = 'Nenhum valor preenchido'; statusEl.style.color = '#f44336';"
    "      return;"
    "    }"
    "    console.log('Enviando dados:', data.toString());"
    "    const r = await fetch('/cfg', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: data });"
    "    const text = await r.text();"
    "    console.log('Resposta do servidor:', text);"
    "    if (!r.ok) throw new Error(`Erro HTTP ${r.status}: ${r.statusText}`);"
    "    let j;"
    "    try { j = JSON.parse(text); } catch (e) { throw new Error(`Erro ao parsear JSON: ${e.message}`); }"
    "    statusEl.textContent = j.message; statusEl.style.color = j.status === 'success' ? '#4CAF50' : '#f44336';"
    "    await loadConfig();"
    "  } catch (e) {"
    "    console.error('Erro no POST:', e);"
    "    statusEl.textContent = `Erro ao salvar: ${e.message}`; statusEl.style.color = '#f44336';"
    "    await loadConfig();"
    "  }"
    "});"
    "</script></body></html>";

// ===================== FUNÇÃO PRINCIPAL =====================
int main()
{
    stdio_init_all();
    inicializar_hardware();
    mutex_sensor = xSemaphoreCreateMutex();
    mutex_config = xSemaphoreCreateMutex();

    gpio_set_irq_enabled_with_callback(BTN_1, GPIO_IRQ_EDGE_FALL, true, &manipulador_interrupcao_gpio);
    gpio_set_irq_enabled(BTN_2, GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(BTN_3, GPIO_IRQ_EDGE_FALL, true);

    xTaskCreate(tarefa_leitura_sensores, "LeituraSensores", 1024, NULL, 2, NULL);
    xTaskCreate(tarefa_webserver, "WebServer", 2048, NULL, 3, NULL);
    xTaskCreate(tarefa_alerta, "Alerta", 1024, NULL, 2, NULL);
    xTaskCreate(tarefa_display, "Display", 1024, NULL, 2, NULL);
    xTaskCreate(tarefa_timeout, "Timeout", 512, NULL, 1, NULL);

    vTaskStartScheduler();
    while (1)
    {
        tight_loop_contents();
    }
    return 0;
}

// ===================== INICIALIZAÇÃO DE HARDWARE =====================
void inicializar_hardware(void)
{
    i2c_init(I2C_PORT_SENSORES, 400 * 1000);
    gpio_set_function(I2C_SENS_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SENS_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SENS_SDA);
    gpio_pull_up(I2C_SENS_SCL);

    i2c_init(I2C_PORT_DISPLAY, 400 * 1000);
    gpio_set_function(I2C_DISP_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_DISP_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_DISP_SDA);
    gpio_pull_up(I2C_DISP_SCL);

    bmp280_init(I2C_PORT_SENSORES);

    inicializar_display();
    inicializar_leds();
    inicializar_buzzer();
    inicializar_botoes();
    if (cyw43_arch_init() != 0)
    {
        printf("[ERRO] Falha ao inicializar cyw43_arch.\n");
        gpio_put(LED_BLUE_PIN, 1);
        return;
    }
    cyw43_arch_enable_sta_mode();
}

void inicializar_display(void)
{
    ssd1306_init(&display, 128, 64, false, DISPLAY_ADDRESS, I2C_PORT_DISPLAY);
    ssd1306_config(&display);
    ssd1306_fill(&display, false);
    ssd1306_send_data(&display);
    sleep_ms(100);
    printf("[INFO] Display OLED inicializado.\n");
}

void inicializar_leds(void)
{
    gpio_init(LED_RED_PIN);
    gpio_set_dir(LED_RED_PIN, GPIO_OUT);
    gpio_put(LED_RED_PIN, 0);
    gpio_init(LED_GREEN_PIN);
    gpio_set_dir(LED_GREEN_PIN, GPIO_OUT);
    gpio_put(LED_GREEN_PIN, 0);
    gpio_init(LED_BLUE_PIN);
    gpio_set_dir(LED_BLUE_PIN, GPIO_OUT);
    gpio_put(LED_BLUE_PIN, 0);
}

void inicializar_buzzer(void)
{
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(BUZZER_PIN);
    pwm_set_clkdiv(slice, PWM_DIVISOR);
    pwm_set_wrap(slice, PWM_WRAP_VALUE);
    pwm_set_gpio_level(BUZZER_PIN, 0);
    pwm_set_enabled(slice, true);
}

void inicializar_botoes(void)
{
    gpio_init(BTN_1);
    gpio_set_dir(BTN_1, GPIO_IN);
    gpio_pull_up(BTN_1);
    gpio_init(BTN_2);
    gpio_set_dir(BTN_2, GPIO_IN);
    gpio_pull_up(BTN_2);
    gpio_init(BTN_3);
    gpio_set_dir(BTN_3, GPIO_IN);
    gpio_pull_up(BTN_3);
}

// ===================== DISPLAY OLED =====================
void atualizar_display(void)
{
    char buf1[24], buf2[24], buf3[24], buf4[24];
    ssd1306_fill(&display, false);
    snprintf(buf1, sizeof(buf1), "Temp: %.1f C", sensor_data.temp_aht20);
    snprintf(buf2, sizeof(buf2), "Umid: %.1f %%", sensor_data.hum_aht20);
    snprintf(buf3, sizeof(buf3), "Press: %.1f hPa", sensor_data.press_bmp280);
    snprintf(buf4, sizeof(buf4), "WiFi: %s", wifi_connected ? "OK" : "---");
    ssd1306_draw_string(&display, buf1, 0, 0);
    ssd1306_draw_string(&display, buf2, 0, 16);
    ssd1306_draw_string(&display, buf3, 0, 32);
    ssd1306_draw_string(&display, buf4, 0, 48);
    ssd1306_send_data(&display);
}

// ===================== TAREFA: DISPLAY OLED =====================
void tarefa_display(void *param)
{
    while (1)
    {
        atualizar_display();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ===================== ALERTA =====================
void emitir_alerta(void)
{
    uint slice = pwm_gpio_to_slice_num(BUZZER_PIN);
    for (int i = 0; i < 3; i++)
    {
        pwm_set_gpio_level(BUZZER_PIN, PWM_WRAP_VALUE / 2);
        vTaskDelay(pdMS_TO_TICKS(ALERT_BEEP_DURATION));
        pwm_set_gpio_level(BUZZER_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(ALERT_BEEP_PAUSE));
    }
}

void tarefa_alerta(void *param)
{
    while (1)
    {
        bool alerta = false;
        if (xSemaphoreTake(mutex_sensor, pdMS_TO_TICKS(100)) && xSemaphoreTake(mutex_config, pdMS_TO_TICKS(100)))
        {
            if (sensor_data.temp_aht20 < config.temp_min || sensor_data.temp_aht20 > config.temp_max ||
                sensor_data.hum_aht20 < config.hum_min || sensor_data.hum_aht20 > config.hum_max ||
                sensor_data.press_bmp280 < config.press_min || sensor_data.press_bmp280 > config.press_max)
            {
                alerta = true;
            }
            xSemaphoreGive(mutex_sensor);
            xSemaphoreGive(mutex_config);
        }
        if (alert_active != alerta)
        {
            if (alerta)
            {
                printf("[ALERTA] Parâmetro fora do limite! T:%.1f U:%.1f P:%.1f\n", sensor_data.temp_aht20, sensor_data.hum_aht20, sensor_data.press_bmp280);
                emitir_alerta();
            }
            else
            {
                printf("[INFO] Todos os parâmetros dentro dos limites.\n");
            }
        }
        alert_active = alerta;
        atualizar_led_status();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void atualizar_led_status(void)
{
    if (alert_active)
    {
        gpio_put(LED_RED_PIN, 1);
        gpio_put(LED_GREEN_PIN, 0);
        gpio_put(LED_BLUE_PIN, 0);
    }
    else
    {
        gpio_put(LED_RED_PIN, 0);
        gpio_put(LED_GREEN_PIN, 1);
        gpio_put(LED_BLUE_PIN, wifi_connected ? 0 : 1);
    }
}

// ===================== TAREFA: LEITURA DE SENSORES =====================
void tarefa_leitura_sensores(void *param)
{
    AHT20_Data aht20;
    struct bmp280_calib_param bmp280_calib;

    bmp280_get_calib_params(I2C_PORT_SENSORES, &bmp280_calib);
    printf("[INFO] Parâmetros de calibração BMP280 carregados.\n");

    while (1)
    {
        if (xSemaphoreTake(mutex_sensor, pdMS_TO_TICKS(100)))
        {
            if (aht20_read(I2C_PORT_SENSORES, &aht20))
            {
                sensor_data.temp_aht20 = aht20.temperature + config.temp_offset;
                sensor_data.hum_aht20 = aht20.humidity + config.hum_offset;
            }
            else
            {
                printf("[ERRO] Falha na leitura do AHT20.\n");
                sensor_data.temp_aht20 = 0.0f;
                sensor_data.hum_aht20 = 0.0f;
            }

            int32_t temp_raw = 0, press_raw = 0;
            bmp280_read_raw(I2C_PORT_SENSORES, &temp_raw, &press_raw);

            if (press_raw == 0)
            {
                printf("[ERRO] Falha na leitura do BMP280: pressão bruta zero.\n");
                sensor_data.press_bmp280 = 0.0f;
            }
            else
            {
                sensor_data.press_bmp280 = bmp280_convert_pressure(press_raw, temp_raw, &bmp280_calib) / 100.0f + config.press_offset;
            }

            if (log_medicoes)
            {
                printf("[SENSORES] Temperatura: %.1f°C | Umidade: %.1f%% | Pressão: %.1f hPa\n",
                       sensor_data.temp_aht20, sensor_data.hum_aht20, sensor_data.press_bmp280);
            }

            xSemaphoreGive(mutex_sensor);
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_READ_INTERVAL_MS));
    }
}

// ===================== TAREFA: TIMEOUT DE CONEXÕES =====================
void tarefa_timeout(void *param)
{
    while (1)
    {
        for (int i = 0; i < MAX_CONNECTIONS; i++)
        {
            if (active_connections[i] != NULL)
            {
                if (absolute_time_diff_us(get_absolute_time(), active_connections[i]->timeout) / 1000 > TCP_TIMEOUT_MS)
                {
                    printf("[TIMEOUT] Fechando conexão inativa com %s\n", ipaddr_ntoa(&active_connections[i]->pcb->remote_ip));
                    close_connection(active_connections[i]);
                    active_connections[i] = NULL;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ===================== WEBSERVER =====================
void close_connection(conn_state_t *state)
{
    if (state == NULL || state->pcb == NULL)
        return;

    for (int i = 0; i < MAX_CONNECTIONS; i++)
    {
        if (active_connections[i] == state)
        {
            active_connections[i] = NULL;
            break;
        }
    }

    tcp_arg(state->pcb, NULL);
    tcp_recv(state->pcb, NULL);
    tcp_sent(state->pcb, NULL);
    tcp_err(state->pcb, NULL);
    err_t err = tcp_close(state->pcb);
    if (err != ERR_OK)
    {
        printf("[ERRO] Falha ao fechar conexão: %d\n", err);
    }
    free(state);
}

void send_http_response(struct tcp_pcb *tpcb, const char *header, const char *body, conn_state_t *state)
{
    err_t err;

    err = tcp_write(tpcb, header, strlen(header), TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK)
    {
        printf("[ERRO] Falha ao enviar cabeçalho HTTP: %d\n", err);
        close_connection(state);
        return;
    }

    if (body && strlen(body) > 0)
    {
        state->remaining_data = body;
        state->remaining_len = strlen(body);
        size_t to_send = state->remaining_len > TCP_CHUNK_SIZE ? TCP_CHUNK_SIZE : state->remaining_len;
        err = tcp_write(tpcb, state->remaining_data, to_send, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK)
        {
            printf("[ERRO] Falha ao enviar corpo HTTP: %d\n", err);
            close_connection(state);
            return;
        }
        state->remaining_data += to_send;
        state->remaining_len -= to_send;
    }
    else
    {
        state->remaining_data = NULL;
        state->remaining_len = 0;
    }

    err = tcp_output(tpcb);
    if (err != ERR_OK)
    {
        printf("[ERRO] Falha ao forçar envio TCP: %d\n", err);
        close_connection(state);
        return;
    }

    state->response_sent = true;
}

static err_t webserver_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    conn_state_t *state = (conn_state_t *)arg;
    if (!state)
        return ERR_OK;

    if (state->remaining_len > 0)
    {
        size_t to_send = state->remaining_len > TCP_CHUNK_SIZE ? TCP_CHUNK_SIZE : state->remaining_len;
        err_t err = tcp_write(tpcb, state->remaining_data, to_send, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK)
        {
            printf("[ERRO] Falha ao enviar pedaço restante do corpo HTTP: %d\n", err);
            close_connection(state);
            return ERR_OK;
        }
        state->remaining_data += to_send;
        state->remaining_len -= to_send;

        err = tcp_output(tpcb);
        if (err != ERR_OK)
        {
            printf("[ERRO] Falha ao forçar envio TCP de pedaço: %d\n", err);
            close_connection(state);
            return ERR_OK;
        }
    }
    else
    {
        printf("[WEBSERVER] Dados enviados completamente para %s\n", ipaddr_ntoa(&tpcb->remote_ip));
        close_connection(state);
    }
    return ERR_OK;
}

static void webserver_error(void *arg, err_t err)
{
    conn_state_t *state = (conn_state_t *)arg;
    if (state)
    {
        printf("[WEBSERVER] Erro na conexão com %s: %d\n", ipaddr_ntoa(&state->pcb->remote_ip), err);
        close_connection(state);
    }
}

static void handle_http_request(struct tcp_pcb *tpcb, const char *req, conn_state_t *state)
{
    if (!req || strlen(req) == 0)
    {
        printf("[ERRO] Requisição vazia ou nula\n");
        send_http_response(tpcb, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: 11\r\n\r\nBad Request", NULL, state);
        return;
    }

    printf("[WEBSERVER] Processando requisição de %s: %.50s...\n", ipaddr_ntoa(&tpcb->remote_ip), req);

    const char *cors_headers = "Access-Control-Allow-Origin: *\r\n"
                               "Access-Control-Allow-Methods: GET, POST\r\n"
                               "Access-Control-Allow-Headers: Content-Type\r\n";

    if (strstr(req, "GET /json") != NULL)
    {
        char json[128];
        snprintf(json, sizeof(json), "{\"temp_aht20\":%.1f,\"hum_aht20\":%.1f,\"press_bmp280\":%.1f}",
                 sensor_data.temp_aht20, sensor_data.hum_aht20, sensor_data.press_bmp280);
        char header[256];
        snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n%sContent-Length: %zu\r\nConnection: close\r\n\r\n", cors_headers, strlen(json));
        send_http_response(tpcb, header, json, state);
    }
    else if (strstr(req, "GET /config") != NULL)
    {
        char json[256];
        snprintf(json, sizeof(json), "{\"temp_min\":%.1f,\"temp_max\":%.1f,\"hum_min\":%.1f,\"hum_max\":%.1f,\"press_min\":%.1f,\"press_max\":%.1f,\"temp_offset\":%.1f,\"hum_offset\":%.1f,\"press_offset\":%.1f}",
                 config.temp_min, config.temp_max, config.hum_min, config.hum_max, config.press_min, config.press_max,
                 config.temp_offset, config.hum_offset, config.press_offset);
        char header[256];
        snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n%sContent-Length: %zu\r\nConnection: close\r\n\r\n", cors_headers, strlen(json));
        send_http_response(tpcb, header, json, state);
    }
    else if (strstr(req, "POST /cfg") != NULL)
    {
        const char *body = strstr(req, "\r\n\r\n");
        if (!body)
        {
            printf("[ERRO] Corpo da requisição POST não encontrado\n");
            char header[256];
            snprintf(header, sizeof(header), "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n%sContent-Length: 45\r\nConnection: close\r\n\r\n", cors_headers);
            send_http_response(tpcb, header, "{\"status\":\"error\",\"message\":\"Corpo ausente\"}", state);
            return;
        }

        body += 4;
        char *body_copy = strdup(body);
        if (!body_copy)
        {
            printf("[ERRO] Falha ao alocar memória para corpo da requisição\n");
            char header[256];
            snprintf(header, sizeof(header), "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\n%sContent-Length: 48\r\nConnection: close\r\n\r\n", cors_headers);
            send_http_response(tpcb, header, "{\"status\":\"error\",\"message\":\"Erro interno\"}", state);
            return;
        }

        bool updated = false;
        char response[512] = "{\"status\":\"success\",\"message\":\"Configuração salva\",\"updates\":[],\"errors\":[]}";
        char updates[256] = "[";
        char errors[256] = "[";
        bool first_update = true, first_error = true;

        if (xSemaphoreTake(mutex_config, pdMS_TO_TICKS(100)))
        {
            char *pair = strtok(body_copy, "&");
            while (pair)
            {
                char *key = strtok(pair, "=");
                char *value = strtok(NULL, "=");
                printf("[CONFIG] Recebido par: %s\n", pair);

                if (!key || !value || strlen(value) == 0)
                {
                    printf("[INFO] Ignorando par inválido ou vazio: %s\n", pair ? pair : "nulo");
                    pair = strtok(NULL, "&");
                    continue;
                }

                float val;
                if (sscanf(value, "%f", &val) != 1)
                {
                    printf("[ERRO] Valor inválido para %s: %s\n", key, value);
                    char err_buf[64];
                    snprintf(err_buf, sizeof(err_buf), "{\"field\":\"%s\",\"error\":\"Valor inválido: %s\"}", key, value);
                    if (!first_error) strncat(errors, ",", sizeof(errors) - strlen(errors) - 1);
                    strncat(errors, err_buf, sizeof(errors) - strlen(errors) - 1);
                    first_error = false;
                    pair = strtok(NULL, "&");
                    continue;
                }

                bool valid = false;
                char field_name[32];
                snprintf(field_name, sizeof(field_name), "%s", key);

                if (strcmp(key, "temp_min") == 0)
                {
                    if (val >= -50.0f && val <= 50.0f)
                    {
                        if (val < config.temp_max)
                        {
                            config.temp_min = val;
                            valid = true;
                            printf("[CONFIG] Novo temp_min: %.1f\n", val);
                        }
                        else
                        {
                            printf("[ERRO] temp_min (%.1f) >= temp_max (%.1f)\n", val, config.temp_max);
                            snprintf(field_name, sizeof(field_name), "%s >= temp_max (%.1f)", key, config.temp_max);
                        }
                    }
                }
                else if (strcmp(key, "temp_max") == 0)
                {
                    if (val >= -50.0f && val <= 50.0f)
                    {
                        if (val > config.temp_min)
                        {
                            config.temp_max = val;
                            valid = true;
                            printf("[CONFIG] Novo temp_max: %.1f\n", val);
                        }
                        else
                        {
                            printf("[ERRO] temp_max (%.1f) <= temp_min (%.1f)\n", val, config.temp_min);
                            snprintf(field_name, sizeof(field_name), "%s <= temp_min (%.1f)", key, config.temp_min);
                        }
                    }
                }
                else if (strcmp(key, "hum_min") == 0)
                {
                    if (val >= 0.0f && val <= 100.0f)
                    {
                        if (val < config.hum_max)
                        {
                            config.hum_min = val;
                            valid = true;
                            printf("[CONFIG] Novo hum_min: %.1f\n", val);
                        }
                        else
                        {
                            printf("[ERRO] hum_min (%.1f) >= hum_max (%.1f)\n", val, config.hum_max);
                            snprintf(field_name, sizeof(field_name), "%s >= hum_max (%.1f)", key, config.hum_max);
                        }
                    }
                }
                else if (strcmp(key, "hum_max") == 0)
                {
                    if (val >= 0.0f && val <= 100.0f)
                    {
                        if (val > config.hum_min)
                        {
                            config.hum_max = val;
                            valid = true;
                            printf("[CONFIG] Novo hum_max: %.1f\n", val);
                        }
                        else
                        {
                            printf("[ERRO] hum_max (%.1f) <= hum_min (%.1f)\n", val, config.hum_min);
                            snprintf(field_name, sizeof(field_name), "%s <= hum_min (%.1f)", key, config.hum_min);
                        }
                    }
                }
                else if (strcmp(key, "press_min") == 0)
                {
                    if (val >= 300.0f && val <= 1100.0f)
                    {
                        if (val < config.press_max)
                        {
                            config.press_min = val;
                            valid = true;
                            printf("[CONFIG] Novo press_min: %.1f\n", val);
                        }
                        else
                        {
                            printf("[ERRO] press_min (%.1f) >= press_max (%.1f)\n", val, config.press_max);
                            snprintf(field_name, sizeof(field_name), "%s >= press_max (%.1f)", key, config.press_max);
                        }
                    }
                }
                else if (strcmp(key, "press_max") == 0)
                {
                    if (val >= 300.0f && val <= 1100.0f)
                    {
                        if (val > config.press_min)
                        {
                            config.press_max = val;
                            valid = true;
                            printf("[CONFIG] Novo press_max: %.1f\n", val);
                        }
                        else
                        {
                            printf("[ERRO] press_max (%.1f) <= press_min (%.1f)\n", val, config.press_min);
                            snprintf(field_name, sizeof(field_name), "%s <= press_min (%.1f)", key, config.press_min);
                        }
                    }
                }
                else if (strcmp(key, "temp_offset") == 0)
                {
                    if (val >= -10.0f && val <= 10.0f)
                    {
                        config.temp_offset = val;
                        valid = true;
                        printf("[CONFIG] Novo temp_offset: %.1f\n", val);
                    }
                }
                else if (strcmp(key, "hum_offset") == 0)
                {
                    if (val >= -10.0f && val <= 10.0f)
                    {
                        config.hum_offset = val;
                        valid = true;
                        printf("[CONFIG] Novo hum_offset: %.1f\n", val);
                    }
                }
                else if (strcmp(key, "press_offset") == 0)
                {
                    if (val >= -50.0f && val <= 50.0f)
                    {
                        config.press_offset = val;
                        valid = true;
                        printf("[CONFIG] Novo press_offset: %.1f\n", val);
                    }
                }
                else
                {
                    printf("[ERRO] Parâmetro desconhecido: %s\n", key);
                    snprintf(field_name, sizeof(field_name), "%s (desconhecido)", key);
                }

                if (!valid)
                {
                    char err_buf[64];
                    snprintf(err_buf, sizeof(err_buf), "{\"field\":\"%s\",\"error\":\"Valor fora do intervalo: %.1f\"}", field_name, val);
                    if (!first_error) strncat(errors, ",", sizeof(errors) - strlen(errors) - 1);
                    strncat(errors, err_buf, sizeof(errors) - strlen(errors) - 1);
                    first_error = false;
                }
                else
                {
                    updated = true;
                    char upd_buf[64];
                    snprintf(upd_buf, sizeof(upd_buf), "{\"field\":\"%s\",\"value\":%.1f}", key, val);
                    if (!first_update) strncat(updates, ",", sizeof(updates) - strlen(updates) - 1);
                    strncat(updates, upd_buf, sizeof(updates) - strlen(updates) - 1);
                    first_update = false;
                }

                pair = strtok(NULL, "&");
            }

            strncat(updates, "]", sizeof(updates) - strlen(updates) - 1);
            strncat(errors, "]", sizeof(errors) - strlen(errors) - 1);
            snprintf(response, sizeof(response), "{\"status\":\"%s\",\"message\":\"%s\",\"updates\":%s,\"errors\":%s}",
                     updated ? "success" : "error",
                     updated ? "Configuração salva" : "Nenhum parâmetro válido aplicado",
                     updates, errors);

            if (updated)
            {
                printf("[CONFIG] Configurações aplicadas: Tmin=%.1f, Tmax=%.1f, Hmin=%.1f, Hmax=%.1f, Pmin=%.1f, Pmax=%.1f, Toff=%.1f, Hoff=%.1f, Poff=%.1f\n",
                       config.temp_min, config.temp_max, config.hum_min, config.hum_max,
                       config.press_min, config.press_max, config.temp_offset, config.hum_offset, config.press_offset);
            }
            else
            {
                printf("[CONFIG] Nenhuma configuração aplicada.\n");
            }

            xSemaphoreGive(mutex_config);
        }
        else
        {
            snprintf(response, sizeof(response), "{\"status\":\"error\",\"message\":\"Erro ao acessar configuração\",\"updates\":[],\"errors\":[]}");
            printf("[ERRO] Falha ao obter mutex_config\n");
        }

        free(body_copy);

        char header[256];
        snprintf(header, sizeof(header), "HTTP/1.1 %s\r\nContent-Type: application/json\r\n%sContent-Length: %zu\r\nConnection: close\r\n\r\n",
                 updated ? "200 OK" : "400 Bad Request", cors_headers, strlen(response));
        send_http_response(tpcb, header, response, state);
    }
    else if (strstr(req, "GET /") != NULL || strstr(req, "GET /index.html") != NULL)
    {
        char header[256];
        snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n%sContent-Length: %zu\r\nConnection: close\r\n\r\n", cors_headers, strlen(html_page));
        send_http_response(tpcb, header, html_page, state);
    }
    else
    {
        char header[256];
        snprintf(header, sizeof(header), "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n%sContent-Length: 12\r\nConnection: close\r\n\r\n", cors_headers);
        send_http_response(tpcb, header, "404 Not Found", state);
    }
}

static err_t webserver_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    conn_state_t *state = (conn_state_t *)arg;

    if (!p)
    {
        printf("[WEBSERVER] Conexão fechada pelo cliente %s\n", ipaddr_ntoa(&tpcb->remote_ip));
        close_connection(state);
        return ERR_OK;
    }

    if (p->tot_len > MAX_REQUEST_SIZE)
    {
        printf("[ERRO] Requisição muito grande de %s: %d bytes\n", ipaddr_ntoa(&tpcb->remote_ip), p->tot_len);
        send_http_response(tpcb, "HTTP/1.1 413 Payload Too Large\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: 16\r\n\r\nPayload Too Large", NULL, state);
        pbuf_free(p);
        return ERR_OK;
    }

    state->timeout = make_timeout_time_ms(TCP_TIMEOUT_MS);

    char *req = (char *)calloc(1, p->tot_len + 1);
    if (!req)
    {
        printf("[ERRO] Falha ao alocar memória para requisição de %s\n", ipaddr_ntoa(&tpcb->remote_ip));
        send_http_response(tpcb, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: 22\r\n\r\nInternal Server Error", NULL, state);
        pbuf_free(p);
        return ERR_OK;
    }

    pbuf_copy_partial(p, req, p->tot_len, 0);
    req[p->tot_len] = '\0';
    handle_http_request(tpcb, req, state);
    free(req);
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t webserver_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    if (err != ERR_OK || newpcb == NULL)
    {
        printf("[ERRO] Falha ao aceitar conexão: %d\n", err);
        return ERR_VAL;
    }

    int free_slot = -1;
    for (int i = 0; i < MAX_CONNECTIONS; i++)
    {
        if (active_connections[i] == NULL)
        {
            free_slot = i;
            break;
        }
    }
    if (free_slot == -1)
    {
        printf("[ERRO] Limite de conexões atingido. Rejeitando conexão de %s\n", ipaddr_ntoa(&newpcb->remote_ip));
        tcp_close(newpcb);
        return ERR_MEM;
    }

    conn_state_t *state = (conn_state_t *)calloc(1, sizeof(conn_state_t));
    if (!state)
    {
        printf("[ERRO] Falha ao alocar estado da conexão para %s\n", ipaddr_ntoa(&newpcb->remote_ip));
        tcp_close(newpcb);
        return ERR_MEM;
    }

    state->pcb = newpcb;
    state->timeout = make_timeout_time_ms(TCP_TIMEOUT_MS);
    state->response_sent = false;
    state->remaining_data = NULL;
    state->remaining_len = 0;

    active_connections[free_slot] = state;

    tcp_arg(newpcb, state);
    tcp_recv(newpcb, webserver_recv);
    tcp_sent(newpcb, webserver_sent);
    tcp_err(newpcb, webserver_error);

    printf("[WEBSERVER] Nova conexão aceita de %s\n", ipaddr_ntoa(&newpcb->remote_ip));
    return ERR_OK;
}

void tarefa_webserver(void *param)
{
    printf("[WIFI] Iniciando conexão Wi-Fi...\n");
    int wifi_attempts = 0;
    const int max_wifi_attempts = 5;

    while (wifi_attempts < max_wifi_attempts)
    {
        if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASS, CYW43_AUTH_WPA2_AES_PSK, 10000) == 0)
        {
            wifi_connected = true;
            break;
        }
        printf("[ERRO] Falha na conexão Wi-Fi, tentativa %d/%d\n", wifi_attempts + 1, max_wifi_attempts);
        wifi_attempts++;
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    if (!wifi_connected)
    {
        printf("[ERRO] Não foi possível conectar ao Wi-Fi após %d tentativas.\n", max_wifi_attempts);
        gpio_put(LED_BLUE_PIN, 1);
        return;
    }

    struct tcp_pcb *pcb = tcp_new();
    if (!pcb)
    {
        printf("[ERRO] Falha ao criar PCB do servidor TCP.\n");
        gpio_put(LED_BLUE_PIN, 1);
        return;
    }

    if (tcp_bind(pcb, IP_ADDR_ANY, 80) != ERR_OK)
    {
        printf("[ERRO] Falha ao associar servidor TCP à porta 80.\n");
        tcp_close(pcb);
        gpio_put(LED_BLUE_PIN, 1);
        return;
    }

    pcb = tcp_listen_with_backlog(pcb, MAX_CONNECTIONS);
    if (!pcb)
    {
        printf("[ERRO] Falha ao configurar servidor TCP para escuta.\n");
        gpio_put(LED_BLUE_PIN, 1);
        return;
    }

    tcp_accept(pcb, webserver_accept);
    printf("[WIFI] Conectado! IP: %s\n", ipaddr_ntoa(&netif_default->ip_addr));
    printf("[SERVIDOR] Servidor web disponível em http://%s:80\n", ipaddr_ntoa(&netif_default->ip_addr));

    while (1)
    {
        cyw43_arch_poll();
        if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP)
        {
            wifi_connected = false;
            gpio_put(LED_BLUE_PIN, 1);
            printf("[WIFI] Conexão perdida. Tentando reconectar...\n");
            cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASS, CYW43_AUTH_WPA2_AES_PSK);
            vTaskDelay(pdMS_TO_TICKS(WIFI_RECONNECT_INTERVAL_MS));
            if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP)
            {
                wifi_connected = true;
                gpio_put(LED_BLUE_PIN, 0);
                printf("[WIFI] Reconectado! IP: %s\n", ipaddr_ntoa(&netif_default->ip_addr));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ===================== INTERRUPÇÕES E BOTÕES =====================
void manipulador_interrupcao_gpio(uint gpio, uint32_t eventos)
{
    static uint32_t ultima = 0;
    uint32_t agora = to_ms_since_boot(get_absolute_time());
    if (agora - ultima < 200)
        return;
    ultima = agora;
    tratar_botao(gpio);
}

void tratar_botao(uint btn)
{
    if (btn == BTN_3)
    {
        if (xSemaphoreTake(mutex_config, pdMS_TO_TICKS(100)))
        {
            config.temp_min = TEMP_MIN_DEFAULT;
            config.temp_max = TEMP_MAX_DEFAULT;
            config.hum_min = HUM_MIN_DEFAULT;
            config.hum_max = HUM_MAX_DEFAULT;
            config.press_min = PRESS_MIN_DEFAULT;
            config.press_max = PRESS_MAX_DEFAULT;
            config.temp_offset = 0;
            config.hum_offset = 0;
            config.press_offset = 0;
            xSemaphoreGive(mutex_config);
            printf("[CONFIG] Limites e offsets resetados para padrão saudável.\n");
        }
    }
    else if (btn == BTN_2)
    {
        printf("[BOOTSEL] Entrando em modo BOOTSEL (USB Mass Storage)...\n");
        sleep_ms(100);
        reset_usb_boot(0, 0);
    }
    else if (btn == BTN_1)
    {
        log_medicoes = !log_medicoes;
        printf("[LOG] Logs de medições %s.\n", log_medicoes ? "ATIVADOS" : "DESATIVADOS");
    }
}