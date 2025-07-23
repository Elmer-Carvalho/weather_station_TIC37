# BoardingControl: Sistema de Controle de Acesso à Embarcação

## 📘 Introdução

O **BoardingControl** é um sistema embarcado implementado no microcontrolador **RP2040** da plataforma **BitDogLab**, projetado para controlar o acesso de passageiros em embarcações com capacidade limitada.

O projeto utiliza **FreeRTOS** para gerenciar tarefas concorrentes, integrando periféricos como **LED RGB**, **display OLED SSD1306**, **buzzer PWM** e **três botões** para controle de embarque, desembarque e reset do sistema.

O sistema monitora em tempo real o número de passageiros a bordo, impedindo embarques quando a capacidade máxima é atingida, com feedback visual através de LEDs indicativos e display OLED, além de alertas sonoros. Ideal para aprendizado em sistemas embarcados, FreeRTOS, sincronização com semáforos e controle de acesso.

---

## 📁 Estrutura do Projeto

```
Boarding_Control/
├── boarding_control.c          # Código principal do projeto
├── CMakeLists.txt              # Configuração de build com Pico SDK
└── lib/                        # Bibliotecas auxiliares
    └── ssd1306.h              # Biblioteca para controle do display OLED
```

---

## 🧩 Especificações do Projeto

### 🔌 Periféricos Utilizados

- **LED RGB**: Indica o status da embarcação nos pinos **13 (vermelho)**, **11 (verde)** e **12 (azul)**.
- **Display OLED SSD1306 128x64**: Exibe informações em tempo real via **I2C** (pinos **14 SDA** e **15 SCL**).
- **Buzzer PWM**: Emite alertas sonoros no **pino 21**.
- **Botões**: Três botões para controle do sistema:
  - **Pino 5**: Embarque de passageiros
  - **Pino 6**: Desembarque de passageiros  
  - **Pino 22**: Reset do sistema (via interrupção)

### 🔧 Recursos do MCU (RP2040)

- **GPIO**: Controle de LED RGB e botões com pull-up interno.
- **I2C**: Comunicação com o display OLED a 400kHz.
- **PWM**: Geração de tons para o buzzer.
- **FreeRTOS**: Três tarefas concorrentes (embarque, desembarque, reset).
- **Semáforos**: Controle de capacidade e sincronização entre tarefas.
- **Mutex**: Proteção de acesso concorrente ao display.

---

## 📦 Materiais Necessários

- Placa **BitDogLab** com **RP2040**
- Cabo **micro-USB para USB-A**
- **Protoboard** e fios **jumper**
- **LED RGB** (cátodo comum)
- **Display OLED SSD1306 (I2C)**
- **Buzzer passivo**
- **Três botões** push-button
- **Resistores** (10kΩ para pull-up, se necessário)

---

## 💻 Softwares Utilizados

- **Visual Studio Code** (recomendado)
- **Pico SDK** (versão 2.1.1 ou superior)
- **FreeRTOS-Kernel** (integrado ao projeto)
- **ARM GCC** (compilador C)
- **CMake**
- **Minicom** ou similar (para debug serial)

---

## ⚙️ Como Utilizar

### 🛠️ Configurar o Hardware

| Componente              | Pino no RP2040 |
|-------------------------|----------------|
| LED RGB (Vermelho)      | GP13           |
| LED RGB (Verde)         | GP11           |
| LED RGB (Azul)          | GP12           |
| Display OLED (SDA)      | GP14           |
| Display OLED (SCL)      | GP15           |
| Buzzer                  | GP21           |
| Botão Embarque          | GP5            |
| Botão Desembarque       | GP6            |
| Botão Reset             | GP22           |

#### Conexões:

- Conecte o **LED RGB** aos pinos **13, 11 e 12** (usar resistores de 220–330 Ω se necessário).
- Ligue o **display OLED** aos pinos **14 (SDA)** e **15 (SCL)**.
- Conecte o **buzzer** ao pino **21**.
- Ligue os **botões** aos pinos **5, 6 e 22** (pull-up interno ativado).

---

### ▶️ Operação

1. Conecte a BitDogLab ao computador via **USB**.
2. Carregue o firmware (`boarding_control.uf2`) na placa.
3. O sistema inicia com **0 passageiros** e capacidade máxima de **10 passageiros**.

#### 🚢 Controle de Passageiros:

- **Botão Embarque (GP5)**:
  - Incrementa o número de passageiros.
  - Bloqueia embarque quando capacidade máxima é atingida.
  - Emite beep de alerta se embarcação estiver lotada.

- **Botão Desembarque (GP6)**:
  - Decrementa o número de passageiros.
  - Libera vagas para novos embarques.
  - Não permite valores negativos.

- **Botão Reset (GP22)**:
  - **Via interrupção**: reset instantâneo do sistema.
  - Zera contador de passageiros.
  - Emite beep duplo de confirmação.

#### 🚥 Indicadores Visuais (LED RGB):

- **Azul**: Embarcação vazia (0 passageiros)
- **Verde**: Ocupação normal (1 a 8 passageiros)
- **Amarelo**: Quase lotada (9 passageiros) - vermelho + verde
- **Vermelho**: Lotada (10 passageiros)

#### 📺 Display OLED:

Exibe em tempo real:
- Número atual de passageiros
- Número de vagas livres
- Status da embarcação (VAZIA/NORMAL/QUASE CHEIA/LOTADA)

---

## 🔎 Indicadores

| Indicador     | Função                                    |
|---------------|-------------------------------------------|
| LED RGB       | Status visual da ocupação da embarcação   |
| Display OLED  | Informações detalhadas em tempo real      |
| Buzzer        | Alertas sonoros (lotação/reset)           |
| Saída Serial  | Debug com log de todas as operações       |

---

## 🔧 Configurações do Sistema

| Parâmetro           | Valor Padrão | Descrição                    |
|---------------------|-------------|------------------------------|
| MAX_PASSAGEIROS     | 10          | Capacidade máxima            |
| DEBOUNCE_DELAY      | 20 ms       | Delay para debounce          |
| BEEP_DURATION       | 150 ms      | Duração do beep              |
| BEEP_PAUSE          | 100 ms      | Pausa entre beeps duplos     |
| PWM_WRAP_VALUE      | 4000        | Resolução do PWM do buzzer   |

---

## 🛡️ Recursos de Segurança

- **Debounce em botões**: Evita múltiplos registros acidentais.
- **Semáforo de contagem**: Controla rigorosamente a capacidade.
- **Mutex no display**: Previne corrupção de dados na tela.
- **Interrupção para reset**: Permite reset imediato em emergências.
- **Validação de limites**: Impede valores inválidos de passageiros.

---

## ⚠️ Limitações

- **Capacidade fixa**: Máximo de 10 passageiros (configurável no código).
- **Detecção manual**: Depende da operação manual dos botões.
- **Sem persistência**: Dados perdidos ao reiniciar o sistema.
- **Buzzer simples**: Tom fixo, sem variação de frequência.

---

## 🚀 Melhorias Futuras

- Adicionar **sensor de presença** para detecção automática.
- Implementar **armazenamento persistente** (EEPROM/Flash).
- Incluir **configuração de capacidade** via interface.
- Adicionar **log de eventos** com timestamp.
- Implementar **comunicação wireless** (Wi-Fi) para monitoramento remoto.
- Adicionar **interface web** para controle e monitoramento.
- Incluir **alertas por e-mail/SMS** quando próximo da capacidade.

---

## 🔍 Arquitetura do Software

```
┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
│  Tarefa         │    │  Tarefa         │    │  Tarefa         │
│  Embarque       │    │  Desembarque    │    │  Reset          │
│  (Prioridade 2) │    │  (Prioridade 2) │    │  (Prioridade 3) │
└─────────────────┘    └─────────────────┘    └─────────────────┘
         │                       │                       │
         └───────────────────────┼───────────────────────┘
                                 │
         ┌───────────────────────┼───────────────────────┐
         │                       │                       │
┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
│   Semáforo      │    │     Mutex       │    │   Semáforo      │
│  Capacidade     │    │    Display      │    │  Reset Sistema  │
│   (Contagem)    │    │   (Proteção)    │    │   (Binário)     │
└─────────────────┘    └─────────────────┘    └─────────────────┘
```

---

## 👨‍💻 Autor

Desenvolvido por **Elmer Carvalho** para o projeto **EmbarcaTech**

---

## 📄 Licença

Este projeto está licenciado sob a **licença MIT**. Consulte o arquivo `LICENSE` para mais informações.