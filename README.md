# BLESPloit

ESP32S3 firmware for BLE exploration: device simulation, central role, scanning, and a built-in web UI. Exposes an HTTP server (HTML, static assets, REST) and a JSON WebSocket at `/ws`.

## Requirements

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) (confirmed ESP-IDF 5.3.1)

## Build & flash

Apply the [patches to NimBLE](patches).

```bash
idf.py build flash monitor
```

## Documentation

- [HTTP & WebSocket API](docs/API.md)
- [Lua bindings (device scripts)](docs/LUA.md)

## More information

[BLESPlo.it](https://blesplo.it)

## Author

Slawomir Jasek

## License

This project is licensed under the MIT License — see [LICENSE](LICENSE).