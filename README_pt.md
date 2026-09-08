[ [English](README.md) | Português ]

# SF3000HD-DeskTop

O **SF3000HD-Desktop** é um complemento para o [TreeFrogUI](https://github.com/tzubertowski/TreeFrogUI), que adiciona uma aplicação (denominada `myapp`) projetada para funcionar como um ambiente de trabalho (desktop) completo para a consola.

## 📦 Aplicações Incluídas

*   **`mywm`**: O próprio gestor de janelas (Window Manager) e ambiente de trabalho.
*   **`myterm`**: Um emulador de terminal personalizado para a consola.
*   **`mykeyboard`**: Um teclado virtual embutido, compatível com qualquer aplicação.
*   **`paint`**: Uma simulação simples de desenho (atualmente sem opções de paleta de cores).
*   **`demo`**: Um teste de renderização de gradientes para avaliar a capacidade da consola.

## 🛠️ Instalação e Compilação

**Para instalar (Utilizadores):**
A instalação é muito simples. Basta fazer o download, extrair os ficheiros e colocar tudo na raiz do seu cartão SD. As pastas irão unir-se automaticamente ao sistema existente, substituindo ou adicionando o que for necessário.

**Compatibilidade:** o desktop está atualmente testado apenas com o **TreeFrogUI 1.3.10** (ramo `treefrogui` do tzubertowski) — aparece no separador **Apps** dessa versão. *Cores* mais antigos (ex.: o ramo `master` congelado) não listam aplicações *standalone*.

**Para compilar (Desenvolvedores):**
Para compilar este projeto precisa da *toolchain* da consola, das bibliotecas base e de uma pasta de trabalho `~/sf3000`:
*   [SF3000 Toolchain v0.1](https://github.com/game-de-it/sf3000/releases/tag/sf3000_toolchain_v0.1) — extraia-a **dentro da sua pasta pessoal** (`$HOME`) e corra `relocate-sdk.sh` uma vez; o `build_all.sh` encontra o compilador automaticamente
*   [SDL 1.2.15](https://github.com/libsdl-org/SDL-1.2) — a API clássica SDL 1.2 (não SDL2), compilada **estática** para MIPS (`libSDL.a`)
*   [SDL_ttf 2.0.11](https://github.com/libsdl-org/SDL_ttf-2.0) (`libSDL_ttf.a`) e [SDL_image 1.2.12](https://github.com/libsdl-org/SDL_image-1.2) (`libSDL_image.a`)
*   [FreeType 2.10.4](https://github.com/bebrws/freetype-2.10.4) (`libfreetype.a`)
*   [TreeFrogUI](https://github.com/tzubertowski/TreeFrogUI) — referência para adicionar *cores* personalizados

   > Atalho: se clonaste este repo, as bibliotecas estáticas já vêm incluídas em `libs/` — basta correr `cp -r libs/* ~/sf3000/sdl/` em vez de as compilar tu.

Passos de compilação (esperados pelo `build_all.sh`):

1. Extraia a *toolchain* em qualquer sítio dentro do seu `$HOME` (ex.: `~/mipsel-buildroot-linux-gnu_sdk-buildroot`) e corra `relocate-sdk.sh` uma vez — **obrigatório**, sem isso o compilador não arranca.
2. Crie `~/sf3000/sdl/` com as bibliotecas estáticas MIPS em `lib/` (`libSDL.a`, `libSDL_ttf.a`, `libSDL_image.a`, `libfreetype.a`) e os *headers* em `include/SDL/` e `include/freetype2/`.
   > A *toolchain* só traz as SDL como bibliotecas partilhadas (`.so`); as `.a` estáticas têm de ser compiladas à parte para MIPS.
3. Copie os *sources* — `src/*.c`, `src/*.h` e `src/build_all.sh` — para `~/sf3000/`.
4. Corra `sh ~/sf3000/build_all.sh`. No fim deve ter o `myapp` mais `demo`, `paint`, `myterm` e `mykeyboard`, todos estaticamente ligados.
5. Copie tudo para o cartão SD **em conjunto** (o compositor espera os companheiros): `myapp` → `cubegm/desktop`, e os outros quatro → `cubegm/wm/`.

*Nota:* Este projeto utiliza uma versão modificada de um dos *cores* do TreeFrogUI. Caso pretenda adicionar um *core* personalizado, confira os exemplos já existentes no repositório para garantir a comunicação correta com o `mywm`.

## 📄 Licença e Agradecimentos

Este projeto está protegido pela licença **Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0)**. Sinta-se à vontade para abrir *Issues* ou enviar *Pull Requests*.

Um agradecimento especial a [**tzubertowski**](https://github.com/tzubertowski) por criar e disponibilizar o incrível projeto TreeFrogUI, sem o qual este complemento não seria possível.