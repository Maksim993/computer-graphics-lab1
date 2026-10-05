## Сборка

```bash
cmake --preset msvc-debug
cmake --build build-debug --parallel
```

## Запуск

```bash
./build-debug/Debug/vulkan-starter-app.exe
```

Рабочая папка должна быть корнем проекта — там находится `shaders/`.

## Что проверить

- Perspective / Orthographic
- Position X/Y/Z
- Rotation X/Y/Z
- Scale X/Y/Z
- Auto rotation
- Play / Pause и скорость анимации
- параметры траектории
- цвет Tint
- параметры камеры
- Reset

Вариант: 1 — куб.
