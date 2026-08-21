# GBArdo

GBArdo é um emulador open-source de Game Boy Advance escrito em C++20, 
construído do zero: um interpretador ARM7TDMI, uma PPU cobrindo os 
seis modos de background além de sprites, emulação de save de cartucho 
(SRAM, Flash, EEPROM) e uma implementação de GPIO/RTC para cartuchos 
que usam esse hardware. Renderizado e controlado usando SDL2.

Este projeto não inclui nem exige uma BIOS real da Nintendo. O
despacho de interrupções e as poucas chamadas de BIOS (`SWI`) das quais
os jogos realmente dependem são implementados nativamente em C++ usando
HLE (High Level Emulation) em vez de rodar o firmware proprietário padrão.

Você é responsável por fazer o dump dos seus próprios cartuchos obtidos
legalmente. Nenhuma ROM está incluída nesse repositório.

## Status

Funcionando:

- Interpretador ARM7TDMI: conjuntos de instruções ARM e Thumb completos
- HLE BIOS: trampolim de despacho de interrupções, além das chamadas SWI
  das quais a maioria dos jogos depende (Halt, IntrWait/VBlankIntrWait,
  Div/DivArm, CpuSet/CpuFastSet, descompressão LZ77)
- PPU: os seis modos de background (Modos 0-2 tiled, incluindo
  rotação/escala afim; Modos 3-5 bitmap), sprites OBJ regulares e afins,
  janelas (Win0/Win1/OBJ window), mosaico e efeitos especiais de cor
  (alpha blending, fade de brilho)
- Timers, DMA (incluindo timing de HBlank/VBlank real), entrada do
  teclado, controlador de interrupções
- Timing de instrução real (custos de ciclo S/N/I por tipo de
  instrução, GBATEK), em vez de um custo fixo de 1 ciclo por instrução
- PPU renderizada scanline por scanline (não um frame inteiro de uma
  vez), incluindo o acumulador de ponto de referência afim interno -
  jogos que dependem de efeitos de raster no meio do frame (splits de
  scroll cronometrados por HBlank, trocas de paleta no meio da tela,
  etc.) renderizam corretamente
- Direct Sound (os dois canais de PCM alimentados por DMA) funcionando
- Emulação de save de cartucho: SRAM, Flash (64K/128K), EEPROM
  (512B/8K), detectados automaticamente a partir da ROM e persistidos em
  um arquivo `.sav` ao lado dela
- Emulação da porta GPIO e do chip RTC (relógio de tempo real), para
  cartuchos que usam esse hardware (mais notavelmente a família Pokemon
  Ruby/Sapphire/Emerald)
- Save states completos (todo o estado da CPU/PPU/Timers/DMA/APU/Bus),
  além de uma interface com tela de splash, seleção de jogos e menu de
  pausa

Lacunas conhecidas:

- Algumas chamadas SWI menos comuns não estão implementadas
  (descompressão Huffman, BgAffineSet, chamadas relacionadas a som) -
  jogos que dependem especificamente delas mostrarão gráficos
  quebrados/ausentes ou travarão nesse ponto
- O limite de hardware real de sprites por scanline não é modelado (uma
  linha incomumente carregada de sprites renderiza completa em vez de
  cortar/degradar como no hardware real)
- A compatibilidade varia por jogo. Títulos mais simples/antigos têm
  mais chance de rodar corretamente do que títulos tecnicamente
  ambiciosos (Pokemon Emerald, por exemplo, ainda tem problemas
  significativos de renderização gráfica)

## Compilando

Requer um compilador C++20, CMake 3.20+ e SDL2 (headers de
desenvolvimento).

```
cmake -S . -B build
cmake --build build -j
```

O tipo de build padrão é `Release` (um build `Debug` sem otimização não
consegue sustentar a velocidade real do GBA, que é cerca de 32fps comparados
aos 59.7fps necessários, medido contra 131fps para o mesmo código compilado
em Release). Passe `-DCMAKE_BUILD_TYPE=Debug` explicitamente se precisar
de um build sem otimização, navegável no gdb.

Outras opções do CMake:

- `GBA_BUILD_TESTS` (padrão `ON`): compila a suíte de testes
- `GBA_ENABLE_ASAN` (padrão `OFF`): AddressSanitizer + UBSan, apenas em
  builds Debug

## Executando

```
./build/bin/gba_emulator
```

Sem argumentos, abre a tela de splash e depois a tela de seleção de
jogos, que lista qualquer arquivo `.gba` encontrado (recursivamente) na
pasta `roms/` ao lado de onde o comando foi executado - crie essa pasta
e coloque suas ROMs lá. Passar um caminho de ROM diretamente pula essa
tela e começa a jogar de imediato:

```
./build/bin/gba_emulator caminho/para/rom.gba
```

Os dados de save (se o cartucho usar SRAM/Flash/EEPROM) são escritos em
um arquivo `.sav` ao lado da ROM, com o mesmo nome base do arquivo da
ROM. Save states (o estado completo da máquina, não só a save do
cartucho) usam um arquivo `.state` do mesmo jeito, um slot por ROM.

Mapeamento do teclado:

| Botão do GBA / Ação      | Tecla         |
|---------------------------|---------------|
| A                          | Z             |
| B                          | X             |
| Start                      | Enter         |
| Select                     | Shift direito |
| D-pad                      | Setas         |
| L                          | A             |
| R                          | S             |
| Pausar / menu de pausa     | Esc           |
| Save state rápido          | F5            |
| Load state rápido          | F9            |

O menu de pausa (Esc durante o jogo) também tem as opções Save State /
Load State / Voltar ao Menu, além de Resume.

## Testes

```
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Os testes ligam diretamente com a biblioteca `gba_core` (o mesmo código
que o emulador real executa) e constroem o estado de CPU/Bus/PPU
diretamente, em vez de depender de arquivos de ROM de teste. Veja os
arquivos em `tests/` para a convenção usada.

## Estrutura do projeto

```
src/core/cpu/      Interpretador ARM7TDMI (decoders ARM + Thumb), HLE BIOS, timing de ciclo
src/core/memory/   Bus (mapa de memória central), emulação de save, GPIO/RTC
src/core/io/       Timers, DMA
src/core/ppu/      Picture Processing Unit (renderização de background/sprites)
src/core/apu/      Direct Sound (canais PCM alimentados por DMA)
src/core/          Emulator (dono e condutor da máquina inteira), save states, tipos compartilhados
src/frontend/      Janela SDL2, splash/menu/pausa, seleção de ROM, save states em arquivo
src/main.cpp       Ponto de entrada (só constrói e roda a App de src/frontend/)
tests/             Testes unitários via CTest contra a gba_core
```
