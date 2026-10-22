# `android/` — APK para ARM64 (Fase 5)

Ainda vazio de propósito: os arquivos Gradle/NDK entram na Fase 5, quando puderem
ser compilados e testados. Estrutura planejada (base: `tools/nfsu2-reference/android/`):

```
android/
├── settings.gradle, build.gradle, gradle.properties
└── app/
    ├── build.gradle                  minSdk 29, abiFilters "arm64-v8a"
    └── src/main/
        ├── AndroidManifest.xml
        ├── java/.../LauncherActivity  escolhe a pasta de dados do jogo (SAF), opções
        ├── java/.../GameActivity      SDL2 + superfície Vulkan
        └── cpp/CMakeLists.txt         add_subdirectory da raiz do repositório:
                                       runtime + generated → libre5_recomp.so
```

Pontos que já estão decididos (ver `docs/ARQUITETURA.md` e `docs/PLANO_DO_PROJETO.md`, Fase 5):

- mesma reserva de 4 GiB do guest e o mesmo código gerado do Linux (D2);
- DXVK com WSI de `ANativeWindow`, portando `tools/nfsu2-reference/ports/dxvk_android`;
- `libadrenotools` para drivers Turnip em Adreno;
- preset de CMake `android-arm64` já existe em `CMakePresets.json`.
