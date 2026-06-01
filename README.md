## leash

```bash
meson setup build
meson compile -C build

./build/leash build ubuntu
./build/leash start ubuntu
./build/leash ssh ubuntu
```

instances, builders, cache, logs, and ssh config live in ~/.leash.
