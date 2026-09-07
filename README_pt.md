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

**Para compilar (Desenvolvedores):**
Para compilar este projeto, é obrigatório utilizar a *toolchain* específica da consola, juntamente com as bibliotecas base:
*   [SF3000 Toolchain v0.1](https://github.com/game-de-it/sf3000/releases/tag/sf3000_toolchain_v0.1)
*   [SDL](https://github.com/libsdl-org/SDL)
*   [FreeType (2.10.4)](https://github.com/bebrws/freetype-2.10.4)
*   [TreeFrogUI](https://github.com/tzubertowski/TreeFrogUI)

*Nota:* Este projeto utiliza uma versão modificada de um dos *cores* do TreeFrogUI. Caso pretenda adicionar um *core* personalizado, confira os exemplos já existentes no repositório para garantir a comunicação correta com o `mywm`.

## 📄 Licença e Agradecimentos

Este projeto está protegido pela licença **Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0)**. Sinta-se à vontade para abrir *Issues* ou enviar *Pull Requests*.

Um agradecimento especial a [**tzubertowski**](https://github.com/tzubertowski) por criar e disponibilizar o incrível projeto TreeFrogUI, sem o qual este complemento não seria possível.