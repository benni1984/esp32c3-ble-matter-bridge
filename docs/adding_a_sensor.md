# Anleitung: Neuen BLE-Sensor hinzufügen

Dieses Projekt wurde bewusst so aufgebaut, dass neue Sensoren mit minimalem Aufwand integriert werden können.
Die meisten Sensoren, die das **BTHome v2**-Protokoll verwenden, funktionieren ohne neue Codezeilen
in den Kernkomponenten — man muss nur ein paar Einträge ergänzen.

---

## Fall 1: Der Sensor benutzt BTHome v2 und sendet bekannte Messwerttypen

Einfach den ESP32 in Reichweite des Sensors bringen.
Wird der Sensor erkannt (UUID `0xFCD2` im BLE Advertisement), erscheint er automatisch
als neues Gerät in Apple Home / Home Assistant — ohne jede Codeänderung.

Bereits unterstützte Typen: Temperatur, Luftfeuchtigkeit, Luftdruck, Helligkeit, UV-Index,
Windgeschwindigkeit, Windrichtung, Niederschlag, Batterie.

---

## Fall 2: Der Sensor benutzt BTHome v2, aber mit einer unbekannten Object-ID

### Schritt 1 — Neuen Sensor-Typ in `bthome.h` eintragen

Öffne `components/bthome/include/bthome.h` und füge deinen Typ dem Enum hinzu:

```c
typedef enum {
    SENSOR_BATTERY,
    SENSOR_TEMPERATURE,
    // ... bestehende Typen ...
    SENSOR_CO2,          // ← neu hinzufügen
    SENSOR_TYPE_COUNT    // muss immer das letzte Element bleiben!
} sensor_type_t;
```

Trage außerdem einen Namen in `sensor_type_name()` in `bthome.cpp` ein (gleiche Position wie im Enum).

### Schritt 2 — Object-ID in `bthome.cpp` eintragen

Öffne `components/bthome/bthome.cpp` und ergänze die Tabelle `s_objects[]`:

```c
// obj_id  type          factor  bytes  signed
{ 0x12,  SENSOR_CO2,   1.0f,   2,     false },
```

Dabei ist:
- `obj_id` — die BTHome Object-ID (aus der [BTHome-Spezifikation](https://bthome.io/format/))
- `factor` — Rohwert × Faktor = physikalischer Wert
- `bytes` — Anzahl Payload-Bytes dieses Objekts
- `signed` — `true` wenn das Vorzeichen berücksichtigt werden muss

### Schritt 3 — Matter-Mapping in `matter_bridge.cpp` ergänzen

Öffne `components/matter_bridge/matter_bridge.cpp` und ergänze zwei `switch`-Blöcke:

**Im `create_sensor_endpoint()`-Switch:**
```cpp
case SENSOR_CO2: {
    // CO2 hat keinen eigenen Matter-Cluster in 1.3.
    // Wir verwenden FlowMeasurement als generischen Platzhalter.
    flow_sensor::config_t cfg;
    cfg.flow_measurement.measured_value = (uint16_t)(initial_value);
    ep = flow_sensor::create(s_node, &cfg, ENDPOINT_FLAG_BRIDGE, s_aggregator);
    break;
}
```

**Im `matter_bridge_update()`-Switch:**
```cpp
case SENSOR_CO2:
    cluster_id = chip::app::Clusters::FlowMeasurement::Id;
    attr_id    = chip::app::Clusters::FlowMeasurement::Attributes::MeasuredValue::Id;
    val        = esp_matter_uint16((uint16_t)(r.value));
    break;
```

Das war's. Neu bauen (`idf.py build`), flashen, fertig.

---

## Fall 3: Der Sensor benutzt ein anderes Protokoll (nicht BTHome)

Beispiel: proprietäres Gerät mit eigenem Advertisement-Format.

### Schritt 1 — Eigenen Parser schreiben

Erstelle eine neue Komponente unter `components/sensors/mein_sensor/`:

```
components/sensors/mein_sensor/
├── CMakeLists.txt
├── include/mein_sensor.h
└── mein_sensor.cpp
```

Dein Parser bekommt die rohen Advertisement-Bytes und füllt eine `sensor_data_t`-Struktur:

```c
bool mein_sensor_parse(const uint8_t *adv_data, size_t len,
                        const uint8_t mac[6], sensor_data_t *out);
```

### Schritt 2 — In `ble_scanner` registrieren

Alternativ kannst du in `ble_scanner.cpp` neben dem BTHome-UUID-Filter einen zweiten
Filter für die Manufacturer-Specific-ID deines Sensors hinzufügen.

### Schritt 3 — In `main.cpp` einbinden

Rufe deinen Parser im `on_ble_advertisement()`-Callback auf,
bevor oder nachdem `bthome_parse()` versucht wird:

```cpp
if (!bthome_parse(svc_data, svc_data_len, &data)) {
    if (!mein_sensor_parse(raw_adv, raw_len, mac, &data)) return;
}
matter_bridge_update(mac, &data);
```

---

## Hinweise

- **Verschlüsselte BTHome-Pakete** werden aktuell übersprungen.
  Entschlüsselung erfordert einen geräteindividuellen Schlüssel; Anleitung folgt in einem späteren Release.
- **Apple Home** zeigt nur Matter-Standardcluster an (Temperatur, Feuchte, Druck, Helligkeit).
  Wind, Regen etc. erscheinen nur in Home Assistant unter dem Bridge-Gerät.
- **Maximal 16 Sensoren** gleichzeitig (Konstante `REGISTRY_MAX_SENSORS` in `sensor_registry.h`).
