# Smart Meter IoT - Monitoramento de Energia (UFCG)

Medidor de energia elétrica residencial construído sobre um chip dedicado de
metrologia polifásica, o **ATM90E36A**, com arquitetura de microcontroladores
duplos: um ATMega328P cuida da medição e um ESP8266 cuida da rede.

As leituras vão por MQTT para um servidor, são armazenadas em série temporal no
PostgreSQL/TimescaleDB e visualizadas no Grafana. O sistema é calibrado contra o
medidor da concessionária e acompanha o consumo com precisão suficiente para
identificar aparelhos individuais e avaliar a qualidade da tensão fornecida.

![Painel de Monitoramento Grafana](/images/Screenshot_20260503_150725.png)

---

## Arquitetura

O projeto separa responsabilidades entre dois microcontroladores:

**ATMega328P — medição.** Lê os registradores do ATM90E36A por SPI, acumula
amostras numa janela de 60 segundos, calcula as médias e envia um pacote JSON
pela serial. Também lê o acumulador de energia do chip, que integra o consumo
por hardware.

**ESP8266 — rede.** Recebe o JSON, gerencia o Wi-Fi, publica no broker MQTT,
atende comandos remotos, guarda leituras em memória flash quando a rede cai e
se atualiza pelo ar.

![Diagrama de Topologia MQTT](/images/broker.png)

| Tópico | Sentido | Conteúdo |
|---|---|---|
| `/UFCG/pwrc/` | medidor para servidor | JSON com as grandezas medidas |
| `/UFCG/pwrc/log` | medidor para servidor | diagnóstico e respostas de comando |
| `MEDIDOR_UFCG_CONTROLE_CASA` | servidor para medidor | comandos remotos |

---

## O que o sistema faz

**Mede** tensão, corrente, potência ativa, reativa e aparente, fator de potência,
frequência e corrente de neutro, nas três fases.

**Acumula energia por hardware.** O ATM90E36A integra o consumo internamente, e o
medidor publica esse contador. Isso torna a contabilidade de energia imune a
falhas de rede: se uma leitura se perde, a seguinte já traz o total acumulado,
como o hodômetro de um carro.

**Recupera dados.** Leituras produzidas enquanto o servidor está inacessível são
gravadas em memória flash e reenviadas quando a conexão volta, cobrindo
interrupções de até três horas.

**Atualiza pelo ar.** O firmware do ESP8266 é substituído remotamente por HTTP,
sem conexão física.

**Calibra remotamente.** Os ganhos de tensão e corrente são ajustados por comandos
MQTT e gravados na EEPROM do ATMega, sobrevivendo a quedas de energia e a
regravações de firmware.

**Espelha o medidor da concessionária.** Um painel acumula energia a partir de uma
leitura conferida, reproduzindo o display do medidor oficial.

---

## Calibração

A precisão do sistema é ancorada no **medidor da concessionária**, que é aferido
legalmente. O método usa a contagem dos pulsos do LED desse medidor, cujo valor em
watt-hora por pulso vem impresso na placa do equipamento:

```
P_real = (N - 1) x 3600 / T      [W]
```

Onde `N` é o número de pulsos contados e `T` o tempo entre o primeiro e o último.
Comparando com a potência que o nosso medidor reporta no mesmo intervalo, obtém-se
o fator de correção a aplicar no ganho.

Esse método foi escolhido em vez de instrumentos de mão porque não depende da
exatidão deles: o valor em watt-hora por pulso é exato por definição, e o único
erro é o do cronômetro. Um alicate amperímetro comum, em corrente baixa, acumula
incerteza da ordem de 14%.

O resultado é verificado por comparação de energia acumulada ao longo de dias
contra a leitura do display. Neste medidor, a calibração corrigiu um erro de 13,6%
e o resíduo ficou em torno de 2%, que é o limite de exatidão do próprio medidor de
referência, classe B.

---

## Comandos Remotos

Publicados no tópico de controle. A carga útil deve conter o comando completo,
com o `Q` inicial e o `M` final.

| Comando | Efeito |
|---|---|
| `QCALIB_GAINSM` | Relata os ganhos gravados na EEPROM |
| `QCALIB_VA:<volts>M` | Calibra a tensão pelo valor de referência |
| `QCALIB_IA:<amperes>M` | Calibra a corrente pelo valor de referência |
| `QCALIB_SETGV:<ganho>M` | Grava um ganho de tensão já calculado |
| `QCALIB_SETGI:<ganho>M` | Grava um ganho de corrente já calculado |
| `QRESETM` | Reinicia o ATMega |
| `RESET_ESP` | Reinicia o ESP8266 |
| `ATUALIZAR` | Dispara a atualização de firmware pelo ar |

Existem variantes `_ALL` e por fase (`VB`, `VC`, `IB`, `IC`) para instalações
polifásicas.

---

## Limitações Conhecidas

**O firmware do ATMega não é atualizável remotamente.** O ESP está ligado a pinos
que o bootloader do ATMega não escuta, e a linha de reset corre no sentido
contrário. Habilitar isso exigiria modificação de placa. A calibração, por outro
lado, é inteiramente remota.

**A comunicação MQTT não é cifrada.**

**A hora gravada no banco é a de chegada**, não a da medição. As leituras já
carregam o instante em que foram medidas, mas a troca da fonte do carimbo de tempo
ainda não foi feita.

---

## Tecnologias

C++ (Arduino CLI) · ATM90E36A · PubSubClient · ArduinoJson · LittleFS ·
ESP8266httpUpdate · Mosquitto · Telegraf · PostgreSQL/TimescaleDB · Grafana

Detalhes de implementação, armadilhas do hardware e o raciocínio por trás das
decisões estão em [docs/notas-tecnicas.md](docs/notas-tecnicas.md).
