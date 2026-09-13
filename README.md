# Smart Meter IoT - Monitoramento de Energia (UFCG)

Sistema de medição de energia elétrica IoT, construído sobre uma arquitetura de
microcontroladores duplos (ATMega328P + ESP8266) e o chip dedicado de metrologia
polifásica **ATM90E36A**.

O sistema coleta, processa e envia dados elétricos para um broker MQTT,
armazenando as séries temporais em PostgreSQL/TimescaleDB para visualização e
faturamento através do Grafana.

## Visão Geral do Dashboard (Grafana)

![Painel de Monitoramento Grafana](/images/Screenshot_20260503_150725.png)

*Painel exibindo as medições instantâneas e o faturamento acumulado do mês.*

---

## Arquitetura do Sistema

O projeto utiliza o princípio de **desacoplamento entre produtor e consumidor**:

* **ATMega328P (produtor / metrologia):** dedicado à leitura dos registradores do
  ATM90E36A via SPI. Acumula amostras numa janela fixa de 60 segundos, calcula as
  médias, aplica o filtro de vazio e envia um pacote JSON via serial a 19200 baud.
* **ESP8266 (consumidor / gateway):** recebe o JSON do ATMega, gerencia a conexão
  Wi-Fi, publica no broker MQTT, escuta comandos remotos, mantém a fila de
  recuperação em memória flash e gerencia as atualizações OTA.

### Topologia de Rede MQTT

A comunicação é baseada numa arquitetura *hub-and-spoke* centrada no broker
Mosquitto, o que mantém o fluxo de **dados de energia** isolado do fluxo de
**comandos remotos**.

![Diagrama de Topologia MQTT](/images/broker.png)

### Tópicos

| Tópico | Sentido | Conteúdo |
|---|---|---|
| `/UFCG/pwrc/` | medidor para servidor | JSON com as 21 grandezas medidas |
| `/UFCG/pwrc/log` | medidor para servidor | diagnóstico, respostas de comando |
| `MEDIDOR_UFCG_CONTROLE_CASA` | servidor para medidor | comandos remotos |

---

## Principais Recursos

* **Medição completa:** tensão (V), corrente (A), potência ativa (W), reativa
  (var), aparente (VA), fator de potência, frequência e corrente de neutro, nas
  três fases.
* **Filtro de vazio:** abaixo de 0,05 A por fase, as grandezas daquela fase são
  zeradas para evitar o cômputo de consumo fantasma a partir do ruído dos sensores.
* **Calibração remota:** ajuste dos ganhos de tensão e corrente por comandos MQTT,
  com cálculo automático do multiplicador e gravação permanente na EEPROM do
  ATMega.
* **Atualização OTA:** o firmware do ESP8266 é atualizado remotamente via HTTP,
  sem conexão USB.
* **Recuperação de dados:** leituras produzidas durante quedas de rede são
  gravadas em memória flash e reenviadas quando a conexão volta.
* **Leitura acumulada:** painel que espelha o display do medidor da
  concessionária, acumulando a partir de uma âncora conferida.

---

## Comandos Remotos (MQTT)

Publicados no tópico de controle do medidor. **A carga útil deve conter o comando
completo**, com o `Q` inicial e o `M` final: o ESP8266 repassa a mensagem
literalmente para o ATMega, sem encapsular nem acrescentar delimitadores.

| Comando | Efeito |
|---|---|
| `QCALIB_GAINSM` | Relata os seis ganhos gravados na EEPROM |
| `QCALIB_VA:<volts>M` | Calibra a tensão da fase A pelo valor de referência |
| `QCALIB_IA:<amperes>M` | Calibra a corrente da fase A pelo valor de referência |
| `QCALIB_V_ALL:<volts>M` | Calibra a tensão e replica para as três fases |
| `QCALIB_I_ALL:<amperes>M` | Calibra a corrente e replica para as três fases |
| `QCALIB_SETGV:<ganho>M` | Grava um ganho de tensão bruto, sem medição |
| `QCALIB_SETGI:<ganho>M` | Grava um ganho de corrente bruto, sem medição |
| `QRESETM` | Reinicia o ATMega |
| `RESET` | Atalho: o ESP traduz para `QRESETM` |
| `RESET_ESP` | Reinicia o ESP8266 |
| `ATUALIZAR` | Dispara a verificação e o download OTA |

Os comandos `QCALIB_SETGV` e `QCALIB_SETGI` gravam um ganho já calculado, sem
medir. São o caminho usado pela calibração por contagem de pulsos, descrita
adiante, e também servem para desfazer uma calibração ruim sem precisar
reproduzir a carga de referência.

O `QCALIB_GAINSM` é a única forma confiável de conferir os ganhos: o bootloader
optiboot do Arduino Uno **não implementa leitura de EEPROM**, e o `avrdude`
devolve o conteúdo da flash sem sinalizar erro. Gravar o sketch com `-D` (padrão
do `arduino-cli`) preserva a EEPROM, de modo que os ganhos sobrevivem às
regravações.

### Endereços na EEPROM

| Grandeza | Endereço | Padrão de fábrica |
|---|---|---|
| Ganho de tensão A / B / C | 0 / 4 / 8 | 47751 |
| Ganho de corrente A / B / C | 2 / 6 / 10 | 12890 |

---

## Método de Calibração: Contagem de Pulsos do Medidor da Concessionária

O medidor da concessionária é aferido legalmente e serve como padrão de
referência. O LED dele pisca uma vez a cada `Kh` watt-hora consumidos, valor
impresso na placa de identificação do equipamento. Nos medidores eletrônicos
monofásicos comuns, `Kh = 1 Wh/pulso`.

### Por que os pulsos e não um instrumento de mão

Um alicate amperímetro típico especifica `±(2% da leitura + 5 dígitos)`. Na escala
de 100 A com resolução de 0,1 A, o termo de dígitos fixos vale 0,5 A sozinho:

```
4,3 A ± (0,086 + 0,5) = 4,3 ± 0,59 A   ->  ±14% de incerteza
```

Em correntes baixas o instrumento não tem resolução para arbitrar nada. A mesma
especificação a 21 A resulta em ±4,5%, ainda alto para calibrar. Já a contagem de
pulsos não depende de exatidão de instrumento: `Kh` é exato por definição, e o
único erro é o do cronômetro.

### Procedimento

**1. Leia os ganhos atuais.** Publique `QCALIB_GAINSM` e anote o valor de `IA`.
É a única forma confiável de obtê-los, já que o bootloader não lê a EEPROM.

**2. Ligue uma carga alta e estável.** Um chuveiro elétrico é ideal: cerca de
4 kW, resistivo, com fator de potência próximo de 1,0. Quanto maior a corrente,
mais rápido os pulsos e menor o peso do erro de cronometragem. Desligue cargas
que ciclam sozinhas, como geladeira e bomba.

**3. Filme o LED do medidor.** A 4 kW os pulsos vêm a cerca de um por segundo,
rápido demais para contar a olho. O vídeo permite contar com calma e fornece a
duração exata entre o primeiro e o último pulso.

**4. Calcule a potência real.** Contando `N` pulsos e medindo o tempo `T` entre o
primeiro e o último:

```
P_real = (N - 1) x 3600 / T      [W]
```

O `N - 1` importa: entre `N` pulsos existem `N - 1` intervalos. Confundir isso
introduz um erro de cerca de 1%, que é a fonte de engano mais comum do método.
Cronometrar do primeiro ao último pulso, em vez de contar durante um tempo fixo,
elimina a incerteza de ±1 pulso.

**5. Obtenha a leitura do medidor na mesma janela.** A média de `P1` nas amostras
que cobrem o intervalo cronometrado.

**6. Calcule o novo ganho.**

```
fator      = P_real / P_medidor
ganho_novo = ganho_IA x fator
```

**7. Aplique e reinicie.**

```
QCALIB_SETGI:<ganho_novo>M
RESET
```

O `RESET` é obrigatório enquanto o ATMega não for regravado com a correção que
reescreve os seis ganhos: sem ele, a gravação parcial derruba os registradores de
tensão para o padrão de fábrica e a leitura salta de 200 V para cerca de 900 V. A
EEPROM não é afetada, então o reinício restaura tudo corretamente.

### Verificação da tensão

Antes de corrigir a corrente, confirme que a tensão está certa. Meça no barramento
do quadro com um multímetro e compare com o valor de `VA` da amostra do mesmo
minuto. Diferença dentro de cerca de 1% significa que o ganho de tensão está bom e
não deve ser tocado.

Isso não é formalidade. Se a tensão estiver errada e você corrigir apenas a
corrente, a energia passa a bater pelo motivo errado: o produto fica certo com os
dois fatores errados. E a tensão deixa de servir para avaliar a qualidade do
fornecimento, que é justamente onde ela tem valor próprio.

Se a tensão precisar de ajuste, use `QCALIB_VA:<volts>M` com a leitura do
multímetro, seguido de `RESET`.

### Validação de longo prazo

A contagem de pulsos é instantânea. Para confirmar que a correção vale em toda a
faixa de operação, e não apenas na corrente em que foi medida, compare a energia
acumulada: anote a leitura do display, aguarde um período longo e compare o delta
com o somatório das medições.

Quanto maior a janela, menor o peso da resolução de 1 kWh do display. Em 24 horas
a incerteza é de cerca de ±8%; em 48 horas, ±4%; em uma semana, abaixo de ±2%.

Resultados obtidos neste medidor:

| Método | Janela | Razão | Conclusão |
|---|---|---|---|
| Energia acumulada | 54 h | 1,155 | lia 13,4% a menos |
| Contagem de pulsos | instantânea, 21 A | 1,136 | lia 13,6% a menos |
| Energia acumulada, pós-correção | 48 h | 0,95 a 1,03 | dentro do ruído |
| Energia acumulada, pós-correção | 145 h | 0,980 | cerca de 2% a mais |

Os dois métodos anteriores à correção concordaram entre si apesar de medirem de
formas completamente diferentes: um integrando dois dias de consumo, outro
instantâneo a 21 A. O ganho de corrente foi de 11588 para 13162.

O resíduo de 2% após a correção está no limite do que a referência distingue. O
medidor da concessionária é **Classe B**, ou seja, ±2% por especificação:
perseguir exatidão abaixo disso sem um padrão melhor não é medição, é ruído.

### Calibração por instrumento de referência

Os comandos `QCALIB_VA`, `QCALIB_IA` e as variantes `_ALL` calibram a partir de um
valor medido, para uso em bancada onde a carga de referência é a única passando
pelo medidor. O ATMega desativa o watchdog, escreve o ganho de referência no canal,
mede `N` vezes o RMS em modo normal e calcula a razão entre o valor informado e a
média lida.

Numa instalação residencial esse caminho exige cuidado: o alicate precisa abraçar
**o mesmo condutor que o transformador de corrente do medidor abraça**. Medir na
tomada de um aparelho lê apenas aquela carga, enquanto o medidor lê a instalação
inteira.

### Escrita dos ganhos no ATM90E36A

Vale para qualquer caminho de calibração. Os registradores de ganho (`0x61` a
`0x6E`) só aceitam escrita com o chip destravado (`0x60 = 0x5678`), e o checksum em
`0x6F` precisa ser refeito ao final. Além disso, **é obrigatório reescrever os seis
ganhos a cada entrada em modo de configuração**: reescrever apenas parte deles faz
os demais voltarem ao padrão de fábrica.

Os ganhos são gravados na EEPROM do ATMega e recarregados no `setup()`, de modo
que sobrevivem a quedas de energia e a regravações de firmware feitas com `-D`.

---

## Confiabilidade e Recuperação de Dados

### Fila em memória flash

O ATMega produz uma leitura por minuto independentemente do estado da rede. Se o
broker estiver inacessível, o ESP8266 grava o JSON num arquivo em LittleFS e o
reenvia quando a conexão volta.

* Capacidade de 80 KB, cerca de 200 leituras, mais de três horas.
* O ponteiro de leitura é persistido em disco a cada envio confirmado, de modo que
  um reinício no meio da drenagem retoma exatamente de onde parou, sem duplicar
  nem pular leitura. Duplicar é pior que perder: inflaria a energia contabilizada.
* A drenagem só começa após a sessão MQTT completar dez segundos estável, porque
  `publish()` devolve sucesso mesmo escrevendo num socket já morto.
* Só há escrita em flash durante indisponibilidade. Em operação normal o arquivo
  nem existe, o que torna o desgaste desprezível.

O diagnóstico periódico publica a contabilidade completa no tópico de log:

```
RSSI=-35dBm heap=45392 frag=1% up=172414s fila=0 fs=1 perdas=0 lidas=2873 pub=2871 enf=2
```

A identidade `lidas = pub + enf + perdas` deve valer sempre. Se não valer, uma
leitura escapou por um caminho não coberto; se `lidas` sequer incrementa, o
problema está antes, na recepção serial.

### Classificação de lacunas

Uma lacuna no banco pode ter duas origens fisicamente opostas, e tratá-las igual
corrompe a contabilidade de energia:

* **Queda de rede:** o medidor estava alimentado e medindo, a casa consumia, mas
  as leituras não chegaram ao servidor. Essa energia existiu e o medidor da
  concessionária a registrou. Deve ser interpolada.
* **Queda de energia:** o medidor estava sem alimentação e nada foi medido, mas o
  consumo real também foi zero. Os dois medidores concordam em zero. Interpolar
  aqui inventaria consumo.

A view `leitura_relogio` decide por ordem de precedência:

1. **Exceção registrada** na tabela `lacunas_excecao`, quando se sabe o que houve.
2. **Campo `up`** (segundos desde o boot do ESP, presente desde o firmware
   1.0.21). Se o `up` da amostra seguinte é menor que a duração da lacuna, o
   módulo reiniciou dentro dela, o que indica perda de alimentação. Se o `up`
   atravessa a lacuna, o medidor permaneceu de pé e a energia foi consumida.
3. **Duração**, como último recurso para dados anteriores ao 1.0.21: até dez
   minutos interpola, acima disso ignora.

O terceiro critério é um palpite e erra nos extremos, como numa queda de energia
de cinco minutos ou numa queda de rede prolongada. Os dois primeiros são
baseados em evidência.

### Buffer de recepção serial

O buffer padrão do ESP8266 tem 256 bytes e o JSON do ATMega tem cerca de 380.
Qualquer bloqueio do laço principal destruía a leitura por transbordo **antes de
ela chegar ao código**, produzindo perda de dados com a fila indicando zero
pendências. O buffer foi ampliado para 1024 bytes antes da chamada de `begin()`.

### Laço principal não bloqueante

A serial é lida primeiro, em toda iteração. A reconexão de Wi-Fi não bloqueia, e
as tentativas de conexão MQTT usam recuo progressivo (5, 10, 20 e até 30 segundos)
porque `client.connect()` é bloqueante e insistir rápido custa mais leituras do
que salva. O timeout de socket é de 5 segundos.

O `keepAlive` do MQTT é de 20 segundos. Com 60 segundos, a detecção de uma conexão
morta levava até dois minutos, durante os quais as publicações eram escritas no
vazio sem que a fila fosse acionada.

Há uma única salvaguarda de reinício: quinze minutos sem broker.

### Atualização OTA

O comando `ATUALIZAR` apenas marca uma flag; a transferência ocorre no laço
principal, nunca dentro do callback do MQTT, para evitar abrir uma conexão HTTP de
dentro do laço de leitura do PubSubClient.

Dois ajustes tornaram o OTA confiável:

* **`ESPhttpUpdate.setClientTimeout(30000)`** — a biblioteca mantém um timeout
  próprio de 8 segundos que **sobrescreve** o configurado no `WiFiClient`. Numa
  rede mesh, uma travada de 8 segundos no meio de 340 KB é comum e abortava a
  transferência.
* **Rádio sempre ligado durante a transferência** — `WIFI_NONE_SLEEP` apenas
  enquanto baixa, restaurado para `WIFI_MODEM_SLEEP` em todos os caminhos de
  saída. O MQTT tolera o rádio dormindo entre beacons, uma transferência grande
  não.

Há também um limite de três minutos entre tentativas: tentativas repetidas que
falham não liberam toda a memória alocada.

---

## Leitura Acumulada (espelho do medidor da concessionária)

A view `leitura_relogio` reproduz o display do medidor da concessionária,
acumulando energia a partir de uma âncora conferida contra o equipamento físico:

```sql
CREATE OR REPLACE VIEW leitura_relogio AS
WITH d AS (
  SELECT time, "P1",
         EXTRACT(EPOCH FROM time - lag(time) OVER (ORDER BY time)) AS dt,
         lag("P1") OVER (ORDER BY time) AS p_ant
  FROM medicoes
  WHERE "ID" = 'MEDIDOR_UFCG' AND time >= TIMESTAMPTZ '<âncora>'
)
SELECT time,
       <leitura> + SUM(
           "P1"/60000.0
         + CASE WHEN dt > 120 AND dt < 600
                THEN (dt - 60) * ("P1" + p_ant) / 2 / 3600 / 1000
                ELSE 0 END
       ) OVER (ORDER BY time) AS leitura_kwh
FROM d;
```

O tratamento de lacunas segue a ordem de precedência descrita em Classificação de
Lacunas: exceção registrada, campo `up`, e duração como último recurso.

O script `reancorar.sh` recria a view com uma nova leitura e registra a âncora no
histórico. Reancorar periodicamente corrige o atraso que as lacunas introduzem.

No painel do Grafana, a unidade deve ser a customizada `suffix: kWh`. A unidade
nativa `kwatth` reescala automaticamente e exibe 4786 kWh como 4,786 MWh.

---

## Limitações Conhecidas

**O firmware do ATMega não é atualizável remotamente.** O ESP está ligado aos
pinos 2 e 3 do ATMega, via SoftwareSerial, enquanto o bootloader optiboot escuta
na UART de hardware, nos pinos 0 e 1. Além disso, a linha de reset corre no
sentido contrário: é o ATMega que reinicia o ESP. Habilitar OTA do ATMega exigiria
modificação de placa. A calibração, por outro lado, é inteiramente remota.

**O carimbo de tempo do banco ainda é o da chegada.** Desde o firmware 1.0.21 cada
leitura carrega o campo `ts`, com o instante real da medição obtido por NTP no
ESP8266 e injetado antes de publicar ou enfileirar, de modo que uma leitura
recuperada horas depois sabe quando foi medida. Falta apenas configurar
`json_time_key = "ts"` no Telegraf para que a coluna `time` passe a ser a hora da
medição. O passo foi adiado de propósito: se o NTP não sincronizar e o campo vier
ausente, o Telegraf descarta a métrica, o que trocaria uma distorção visual por
perda de dados. A troca deve ser feita após confirmar cobertura integral do campo.

**A constante de conversão dos acumuladores de energia ainda é empírica.** Desde
setembro de 2026 o firmware lê o registrador `0x80` (energia ativa direta total),
que é *read-to-clear*: cada leitura devolve a energia integrada por hardware desde
a anterior. O valor é publicado como `EMIN`, e a soma desde o boot como `EACC`.

O `PL_Constant` (registradores `0x31` e `0x32`) foi fixado em `0x0861C468` para que
a escala seja determinística. A conversão de contagens para kWh, porém, depende
dos ganhos de calibração da instalação, e por isso não é transferível de outros
projetos: ela precisa ser derivada comparando `EACC` com a integral da potência ao
longo de uma janela longa, e refeita sempre que os ganhos mudarem.

Note que `0x48`, `0x4A` e `0x4C`, escritos no `setup()`, são `PhiA`, `PhiB` e
`PhiC` — ângulos de calibração de fase, não constantes de energia.

**A comunicação MQTT não é cifrada.** Usuário e senha trafegam em texto claro na
porta 1883.

---

## Tecnologias e Bibliotecas

* C++ (Arduino CLI)
* ATM90E36A (metrologia polifásica)
* PubSubClient (MQTT)
* ArduinoJson (estruturação de dados)
* LittleFS (fila de recuperação em flash)
* ESP8266httpUpdate (OTA)
* Mosquitto (broker MQTT)
* Telegraf (ingestão MQTT para banco)
* PostgreSQL / TimescaleDB (série temporal)
* Grafana (visualização e queries SQL)
