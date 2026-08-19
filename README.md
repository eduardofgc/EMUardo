# EMUardo

Um emulador de Game Boy Advance escrito em C++20. Possui um
interpretador ARM7TDMI (conjuntos de instruções ARM e Thumb), uma PPU
cobrindo os seis modos de background além de sprites, emulação de save de
cartucho (SRAM, Flash, EEPROM) e uma implementação de GPIO/RTC para
cartuchos que usam esse hardware. Renderizado e controlado via SDL2.

Este projeto não inclui nem exige uma BIOS real da Nintendo. O
despacho de interrupções e as poucas chamadas de BIOS (`SWI`) das quais
os jogos realmente dependem são implementados nativamente em C++ usando 
HLE em vez de rodar o firmware proprietário da Nintendo. 

NENHUMA ROM ESTÁ INCLUÍDA NESSE REPOSITÓRIO. VOCÊ É RESPONSÁVEL
POR FAZER O DUMP DE SUAS PRÓPRIAS ROMS.

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
- Timers, DMA, entrada do teclado, controlador de interrupções
- Emulação de save de cartucho: SRAM, Flash (64K/128K), EEPROM
  (512B/8K), detectados automaticamente a partir da ROM e persistidos em
  um arquivo `.sav` ao lado dela
- Emulação da porta GPIO e do chip RTC (relógio de tempo real), para
  cartuchos que usam esse hardware (mais notavelmente a família Pokemon
  Ruby/Sapphire/Emerald)

Lacunas conhecidas:

- Sem APU/som - o emulador roda em silêncio
- A PPU renderiza um frame inteiro de uma vez em vez de scanline por
  scanline, então jogos que dependem de efeitos de raster no meio do
  frame (splits de scroll cronometrados por HBlank, trocas de paleta no
  meio da tela, etc.) vão renderizar incorretamente
- Sprites ainda não são compostos sobre backgrounds bitmap dos Modos
  3/4/5
- Diversas chamadas SWI menos comuns não estão implementadas
  (descompressão Huffman/RL, Diff8bit/16bitUnFilter, Sqrt, ArcTan,
  BgAffineSet, ObjAffineSet, chamadas relacionadas a som)
- A compatibilidade varia por jogo - títulos mais simples/antigos têm
  mais chance de rodar corretamente do que títulos tecnicamente
  ambiciosos

## Compilando

Requer um compilador C++20, CMake 3.20+ e SDL2 (headers de
desenvolvimento).

```
cmake -S . -B build
cmake --build build -j
```

O tipo de build padrão é `Release` (um build `Debug` sem otimização não
consegue sustentar a velocidade real do GBA: cerca de 32fps contra os
59.7fps necessários, medido contra 131fps para o mesmo código compilado
em Release). Passe `-DCMAKE_BUILD_TYPE=Debug` explicitamente se precisar
de um build sem otimização, navegável no gdb.

Outras opções do CMake:

- `GBA_BUILD_TESTS` (padrão `ON`): compila a suíte de testes
- `GBA_ENABLE_ASAN` (padrão `OFF`): AddressSanitizer + UBSan, apenas em
  builds Debug

## Executando

```
./build/bin/gba_emulator caminho/para/rom.gba
```

Os dados de save (se o cartucho usar SRAM/Flash/EEPROM) são escritos em
um arquivo `.sav` ao lado da ROM, com o mesmo nome base do arquivo da
ROM.

Mapeamento do teclado:

| Botão do GBA | Tecla       |
|---------------|-------------|
| A             | Z           |
| B             | X           |
| Start         | Enter       |
| Select        | Shift direito |
| D-pad         | Setas       |
| L             | A           |
| R             | S           |

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
src/core/cpu/      Interpretador ARM7TDMI (decoders ARM + Thumb), HLE BIOS
src/core/memory/   Bus (mapa de memória central), emulação de save, GPIO/RTC
src/core/io/       Timers, DMA
src/core/ppu/      Picture Processing Unit (renderização de background/sprites)
src/core/          Emulator (dono e condutor da máquina inteira), tipos compartilhados
src/main.cpp       Loop de janela/input/apresentação via SDL2
tests/             Testes unitários via CTest contra a gba_core
```
