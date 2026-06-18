# Protocolo AWS-S3

El cliente conserva una conexión TCP durante toda la ejecución de un comando.
El servidor procesa solicitudes secuencialmente hasta que el cliente cierra la
conexión.

## Encabezado

Todos los enteros viajan en orden de red. El encabezado ocupa exactamente
24 bytes:

| Offset | Tamaño | Campo |
|---:|---:|---|
| 0 | 4 | Magic `0x53334653` (`S3FS`) |
| 4 | 2 | Versión, actualmente `1` |
| 6 | 2 | Código de operación |
| 8 | 4 | Banderas |
| 12 | 4 | Estado; debe ser cero en solicitudes |
| 16 | 8 | Longitud total del payload |

Las cadenas se codifican como longitud `uint32_t` seguida por esa cantidad
exacta de bytes, sin `NUL`. Los archivos se transmiten en bloques de 64 KiB,
pero el tamaño del bloque no forma parte del protocolo.

## Operaciones

| Operación | Solicitud | Respuesta exitosa |
|---|---|---|
| `LIST_BUCKETS` | Sin payload | Manifiesto |
| `LIST_OBJECTS` | bucket, prefijo | Manifiesto |
| `MAKE_BUCKET` | bucket | Vacía |
| `REMOVE_BUCKET` | bucket | Vacía |
| `PUT_OBJECT` | bucket, clave, mtime, tamaño, bytes | Vacía |
| `GET_OBJECT` | bucket, clave | mtime, tamaño, bytes |
| `COPY_OBJECT` | bucket/clave origen y destino | Vacía |
| `MOVE_OBJECT` | bucket/clave origen y destino | Vacía |
| `DELETE_OBJECT` | bucket, clave o prefijo | Vacía |

Un manifiesto contiene un `uint32_t` con la cantidad de entradas y, por cada
entrada: cadena de nombre, `uint64_t size`, `uint64_t mtime` y `uint32_t`
que indica si representa un prefijo.

Si una operación falla, el estado del encabezado es distinto de cero y el
payload contiene una única cadena con el mensaje de error.

