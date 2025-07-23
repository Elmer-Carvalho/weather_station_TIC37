/*
 * Projeto: Estação Meteorológica BitDogLab
 * Plataforma: Raspberry Pi Pico W (BitDogLab)
 * Autor: Elmer Carvalho
 * Descrição: Sistema embarcado para leitura dos sensores AHT20 (temperatura/umidade) e BMP280 (pressão/temperatura),
 *            exibição local em display OLED SSD1306, servidor web responsivo com AJAX, alertas visuais/sonoros e
 *            configuração de limites/offsets via interface web e botões físicos.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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
#define TCP_TIMEOUT_MS 10000 // Timeout de 10 segundos para conexões inativas

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
    float temp_bmp280;
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
void manipulador_interrupcao_gpio(uint gpio, uint32_t eventos);
void tratar_botao(uint btn);
static err_t webserver_sent(void *arg, struct tcp_pcb *tpcb, u16_t len);
static err_t webserver_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static err_t webserver_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
void send_http_response(struct tcp_pcb *tpcb, const char *header, const char *body, conn_state_t *state);

// ===================== HTML/CSS/JS EMBUTIDO =====================
const char *html_page =
    "<!DOCTYPE html><html lang='pt'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>Estação BitDogLab</title>"
    "<style>body{font-family:sans-serif;margin:0;padding:20px;background:#222;color:#eee;}h1{font-size:1.5em;}#dados{margin:1em 0;font-size:1.2em;}input,button{font-size:1em;padding:5px;}@media(max-width:600px){body{font-size:1.1em;padding:10px;}}</style>"
    "</head><body><h1>Estação Meteorológica BitDogLab</h1><div id='dados'>Carregando...</div><canvas id='grafico' width='300' height='100'></canvas>"
    "<form id='cfg'><h2>Configuração</h2>"
    "Temp:<input name='temp_min' type='number' step='0.1' placeholder='Mín'/>-<input name='temp_max' type='number' step='0.1' placeholder='Máx'/><br>"
    "Umid:<input name='hum_min' type='number' step='0.1' placeholder='Mín'/>-<input name='hum_max' type='number' step='0.1' placeholder='Máx'/><br>"
    "Press:<input name='press_min' type='number' step='0.1' placeholder='Mín'/>-<input name='press_max' type='number' step='0.1' placeholder='Máx'/><br>"
    "Offsets: T:<input name='temp_offset' type='number' step='0.1'/> U:<input name='hum_offset' type='number' step='0.1'/> P:<input name='press_offset' type='number' step='0.1'/><br>"
    "<button type='submit'>Salvar</button></form>"
    "<script>let dados=[];function atualiza(){fetch('/json').then(r=>r.json()).then(j=>{document.getElementById('dados').innerHTML=`Temp: ${j.temp_aht20}°C, Umid: ${j.hum_aht20}%, Press: ${j.press_bmp280}hPa`;dados.push(j);if(dados.length>50)dados.shift();let c=document.getElementById('grafico').getContext('2d');c.clearRect(0,0,300,100);c.strokeStyle='#0f0';c.beginPath();for(let i=0;i<dados.length;i++){let y=100-((dados[i].temp_aht20-0)*2);c.lineTo(i*6,y);}c.stroke();}).catch(e=>console.error('Erro:',e));}setInterval(atualiza,2000);atualiza();document.getElementById('cfg').onsubmit=e=>{e.preventDefault();let f=new FormData(e.target);fetch('/cfg',{method:'POST',body:new URLSearchParams(f)}).then(()=>alert('Configuração salva!')).catch(e=>alert('Erro ao salvar configuração.'));};</script></body></html>";

// ===================== FUNÇÃO PRINCIPAL =====================
int main()
{
    stdio_init_all();
    inicializar_hardware();
    mutex_sensor = xSemaphoreCreateMutex();
    mutex_config = xSemaphoreCreateMutex();

    // Configura interrupções dos botões
    gpio_set_irq_enabled_with_callback(BTN_1, GPIO_IRQ_EDGE_FALL, true, &manipulador_interrupcao_gpio);
    gpio_set_irq_enabled(BTN_2, GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(BTN_3, GPIO_IRQ_EDGE_FALL, true);

    // Cria tarefas
    xTaskCreate(tarefa_leitura_sensores, "LeituraSensores", 1024, NULL, 2, NULL);
    xTaskCreate(tarefa_webserver, "WebServer", 2048, NULL, 3, NULL);
    xTaskCreate(tarefa_alerta, "Alerta", 1024, NULL, 2, NULL);
    xTaskCreate(tarefa_display, "Display", 1024, NULL, 2, NULL);

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
    // Inicializa I2C dos sensores
    i2c_init(I2C_PORT_SENSORES, 400 * 1000);
    gpio_set_function(I2C_SENS_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SENS_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SENS_SDA);
    gpio_pull_up(I2C_SENS_SCL);

    // Inicializa I2C do display
    i2c_init(I2C_PORT_DISPLAY, 400 * 1000);
    gpio_set_function(I2C_DISP_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_DISP_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_DISP_SDA);
    gpio_pull_up(I2C_DISP_SCL);

    // Inicializa BMP280
    bmp280_init(I2C_PORT_SENSORES);

    inicializar_display();
    inicializar_leds();
    inicializar_buzzer();
    inicializar_botoes();
    cyw43_arch_init();
    cyw43_arch_enable_sta_mode();
    cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASS, CYW43_AUTH_WPA2_AES_PSK);
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

// ===================== TAREFA: ALERTA =====================
void tarefa_alerta(void *param)
{
    static bool ultimo_alerta = false;
    while (1)
    {
        bool alerta = false;
        if (xSemaphoreTake(mutex_sensor, pdMS_TO_TICKS(100)))
        {
            if (sensor_data.temp_aht20 < config.temp_min || sensor_data.temp_aht20 > config.temp_max ||
                sensor_data.hum_aht20 < config.hum_min || sensor_data.hum_aht20 > config.hum_max ||
                sensor_data.press_bmp280 < config.press_min || sensor_data.press_bmp280 > config.press_max)
            {
                alerta = true;
            }
            xSemaphoreGive(mutex_sensor);
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
        gpio_put(LED_BLUE_PIN, 0);
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
            // Leitura do AHT20
            if (aht20_read(I2C_PORT_SENSORES, &aht20))
            {
                sensor_data.temp_aht20 = aht20.temperature + config.temp_offset;
                sensor_data.hum_aht20 = aht20.humidity + config.hum_offset;
            }
            else
            {
                printf("[ERRO] Falha na leitura do AHT20.\n");
            }

            // Leitura do BMP280
            int32_t temp_raw = 0, press_raw = 0;
            bmp280_read_raw(I2C_PORT_SENSORES, &temp_raw, &press_raw);

            if (press_raw == 0)
            {
                printf("[ERRO] Falha na leitura do BMP280: pressão bruta zero.\n");
            }
            else
            {
                sensor_data.temp_bmp280 = bmp280_convert_temp(temp_raw, &bmp280_calib) / 100.0f + config.temp_offset;
                sensor_data.press_bmp280 = bmp280_convert_pressure(press_raw, temp_raw, &bmp280_calib) / 100.0f + config.press_offset;
            }

            if (log_medicoes)
            {
                printf("[SENSORES] Temperatura: %.1f°C | Umidade: %.1f%% | Pressão: %.1f hPa\n",
                       sensor_data.temp_aht20, sensor_data.hum_aht20, sensor_data.press_bmp280);
                printf("[DEBUG] BMP280 Raw - Temp: %ld, Press: %ld\n", temp_raw, press_raw);
            }

            xSemaphoreGive(mutex_sensor);
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_READ_INTERVAL_MS));
    }
}

// ===================== WEBSERVER =====================
void send_http_response(struct tcp_pcb *tpcb, const char *header, const char *body, conn_state_t *state)
{
    err_t err;

    // Envia o cabeçalho
    err = tcp_write(tpcb, header, strlen(header), TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK)
    {
        printf("[ERRO] Falha ao enviar cabeçalho HTTP: %d\n", err);
        return;
    }

    // Envia o corpo, se existir
    if (body)
    {
        err = tcp_write(tpcb, body, strlen(body), TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK)
        {
            printf("[ERRO] Falha ao enviar corpo HTTP: %d\n", err);
            return;
        }
    }

    // Força o envio dos dados
    err = tcp_output(tpcb);
    if (err != ERR_OK)
    {
        printf("[ERRO] Falha ao forçar envio TCP: %d\n", err);
        return;
    }

    state->response_sent = true; // Marca a resposta como enviada
}

static err_t webserver_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    conn_state_t *state = (conn_state_t *)arg;
    if (state && state->response_sent)
    {
        printf("[WEBSERVER] Dados enviados, fechando conexão com %s\n", ipaddr_ntoa(&tpcb->remote_ip));
        tcp_close(tpcb);
        free(state);
    }
    return ERR_OK;
}

static void webserver_error(void *arg, err_t err)
{
    conn_state_t *state = (conn_state_t *)arg;
    if (state)
    {
        printf("[WEBSERVER] Erro na conexão: %d\n", err);
        free(state);
    }
}

static void handle_http_request(struct tcp_pcb *tpcb, const char *req, conn_state_t *state)
{
    if (strstr(req, "GET /json") != NULL)
    {
        char json[128];
        snprintf(json, sizeof(json), "{\"temp_aht20\":%.1f,\"hum_aht20\":%.1f,\"press_bmp280\":%.1f}",
                 sensor_data.temp_aht20, sensor_data.hum_aht20, sensor_data.press_bmp280);
        send_http_response(tpcb, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n", json, state);
    }
    else if (strstr(req, "POST /cfg") != NULL)
    {
        const char *body = strstr(req, "\r\n\r\n");
        if (body)
        {
            body += 4;
            float tmin, tmax, hmin, hmax, pmin, pmax, toff, hoff, poff;
            sscanf(body, "temp_min=%f&temp_max=%f&hum_min=%f&hum_max=%f&press_min=%f&press_max=%f&temp_offset=%f&hum_offset=%f&press_offset=%f",
                   &tmin, &tmax, &hmin, &hmax, &pmin, &pmax, &toff, &hoff, &poff);
            if (xSemaphoreTake(mutex_config, pdMS_TO_TICKS(100)))
            {
                config.temp_min = tmin;
                config.temp_max = tmax;
                config.hum_min = hmin;
                config.hum_max = hmax;
                config.press_min = pmin;
                config.press_max = pmax;
                config.temp_offset = toff;
                config.hum_offset = hoff;
                config.press_offset = poff;
                xSemaphoreGive(mutex_config);
                printf("[CONFIG] Limites e offsets atualizados via web.\n");
            }
        }
        send_http_response(tpcb, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n", "OK", state);
    }
    else
    {
        char header[128];
        snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\nContent-Length: %zu\r\n\r\n", strlen(html_page));
        send_http_response(tpcb, header, html_page, state);
    }
}

static err_t webserver_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    conn_state_t *state = (conn_state_t *)arg;

    if (!p)
    {
        printf("[WEBSERVER] Conexão fechada pelo cliente %s\n", ipaddr_ntoa(&tpcb->remote_ip));
        tcp_close(tpcb);
        if (state)
            free(state);
        return ERR_OK;
    }

    // Atualiza o timeout da conexão
    state->timeout = make_timeout_time_ms(TCP_TIMEOUT_MS);

    printf("[WEBSERVER] Requisição recebida de %s\n", ipaddr_ntoa(&tpcb->remote_ip));
    char *req = (char *)calloc(1, p->tot_len + 1);
    if (!req)
    {
        printf("[ERRO] Falha ao alocar memória para requisição.\n");
        pbuf_free(p);
        tcp_close(tpcb);
        if (state)
            free(state);
        return ERR_MEM;
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

    // Aloca estado para a nova conexão
    conn_state_t *state = (conn_state_t *)calloc(1, sizeof(conn_state_t));
    if (!state)
    {
        printf("[ERRO] Falha ao alocar estado da conexão.\n");
        tcp_close(newpcb);
        return ERR_MEM;
    }

    state->pcb = newpcb;
    state->timeout = make_timeout_time_ms(TCP_TIMEOUT_MS);
    state->response_sent = false;

    // Configura callbacks
    tcp_arg(newpcb, state);
    tcp_recv(newpcb, webserver_recv);
    tcp_sent(newpcb, webserver_sent);
    tcp_err(newpcb, webserver_error);

    printf("[WEBSERVER] Nova conexão aceita de %s\n", ipaddr_ntoa(&newpcb->remote_ip));
    return ERR_OK;
}

void tarefa_webserver(void *param)
{
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb)
    {
        printf("[ERRO] Falha ao criar PCB do servidor TCP.\n");
        return;
    }

    if (tcp_bind(pcb, IP_ADDR_ANY, 80) != ERR_OK)
    {
        printf("[ERRO] Falha ao associar servidor TCP à porta 80.\n");
        tcp_close(pcb);
        return;
    }

    pcb = tcp_listen_with_backlog(pcb, 4);
    if (!pcb)
    {
        printf("[ERRO] Falha ao configurar servidor TCP para escuta.\n");
        return;
    }

    tcp_accept(pcb, webserver_accept);
    printf("[WIFI] Aguardando conexão Wi-Fi...\n");

    while (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP)
    {
        cyw43_arch_poll();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    wifi_connected = true;
    if (netif_default)
    {
        printf("[WIFI] Conectado! IP: %s\n", ipaddr_ntoa(&netif_default->ip_addr));
        printf("[SERVIDOR] Servidor web disponível em http://%s:80\n", ipaddr_ntoa(&netif_default->ip_addr));
    }
    else
    {
        printf("[ERRO] Não foi possível obter o IP.\n");
    }

    while (1)
    {
        cyw43_arch_poll();
        vTaskDelay(pdMS_TO_TICKS(50)); // Ajustado para 50ms
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