# Protocolo AWS-S3

El cliente conserva una conexión TCP durante toda la ejecución de un comando.
El servidor procesa solicitudes secuencialmente hasta que el cliente cierra la
conexión.

Este protocolo es interno del proyecto. No utiliza HTTP: se definió un formato
binario propio para poder enviar metadatos y archivos de cualquier tipo sin
confundir el contenido con los comandos.

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

## Ejemplo de una subida

Para ejecutar:

```sh
bin/aws-s3 cp foto.jpg s3://imagenes/vacaciones/foto.jpg
```

ocurre lo siguiente:

1. El cliente abre `foto.jpg` y obtiene su tamaño y fecha.
2. Envía un encabezado con la operación `PUT_OBJECT`.
3. Envía el bucket, la clave, la fecha y el tamaño.
4. Envía el contenido en bloques de 64 KiB.
5. El servidor guarda el objeto y responde con `STATUS_OK` o un error.

## Ejemplo de una descarga

Para una descarga, el cliente envía `GET_OBJECT` junto con el bucket y la
clave. La respuesta contiene la fecha, el tamaño y luego los bytes del archivo.
El cliente crea la ruta local necesaria y conserva la fecha de modificación.

## Operaciones de alto nivel

Algunos comandos requieren varias solicitudes:

- Un `mv` equivale a copiar y después eliminar.
- Un comando recursivo envía una operación por cada archivo encontrado.
- `sync` solicita manifiestos, compara tamaño y fecha, y transfiere solamente
  los archivos nuevos o modificados.
- `sync --delete` elimina los elementos sobrantes después de completar las
  copias.

## Aspectos útiles para las pruebas

- Una llamada a `read` puede recibir solamente una parte del mensaje. Por eso
  se utilizan `read_full` y `write_full`.
- El protocolo debe funcionar con archivos que contengan cualquier byte,
  incluidos bytes nulos.
- Los tamaños utilizan enteros de 64 bits.
- Una respuesta de error contiene un estado distinto de cero y un mensaje.
