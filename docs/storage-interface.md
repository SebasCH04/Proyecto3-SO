# Contrato para el motor de almacenamiento

El archivo público `include/storage.h` es la frontera entre ambas mitades del
proyecto. `src/server.c` solo llama esas funciones y no conoce el formato
interno de los buckets.

El backend persistente está implementado en `src/storage_file.c`. Este:

1. Implementa todas las funciones declaradas en `storage.h`.
2. Consume exactamente `size` bytes de `input_fd` en `storage_put_object`.
3. Entrega en `storage_reader_t` un descriptor legible, desplazamiento,
   tamaño y fecha. El servidor usa `pread`, por lo que no depende de la
   posición actual del descriptor.
4. Devuelve uno de los estados definidos en `protocol.h` y escribe un mensaje
   breve en el buffer `error`.
5. Mantiene vigente el descriptor del lector hasta `storage_close_reader`.

Cada bucket reserva un bloque de directorio de 1 MiB para un encabezado
versionado, hasta 1024 objetos y hasta 1024 espacios libres. La implementación
usa primer ajuste, divide bloques sobrantes, une espacios contiguos y reduce el
archivo cuando el espacio libre alcanza el final. Los campos se serializan
explícitamente en orden de red.

## Distribución de un archivo bucket

```text
Byte 0
+------------------------------+
| Encabezado del bucket        |
+------------------------------+
| Tabla de objetos             |
+------------------------------+
| Lista de espacios libres     |
+------------------------------+  Byte 1048576
| Datos del objeto 1           |
+------------------------------+
| Datos del objeto 2           |
+------------------------------+
| ...                          |
+------------------------------+
```

El encabezado contiene la identificación `S3BUCK01`, la versión, las
capacidades de las tablas, sus contadores y la posición donde termina el área
de datos.

Cada entrada de objeto guarda:

- Clave.
- Posición absoluta dentro del archivo.
- Tamaño.
- Fecha de modificación.

Cada entrada libre guarda una posición y un tamaño.

## Creación y carga

Al crear un bucket se genera `data/<bucket>.s3b` y se reserva el primer MiB.
Cuando el servidor abre un bucket existente, lee los metadatos y valida que:

- El formato y la versión sean conocidos.
- Los conteos no superen los límites.
- Los objetos estén dentro del archivo.
- No existan objetos superpuestos.
- Los espacios libres no se superpongan con objetos.

## Escritura y reemplazo

Para un objeto nuevo se recorre la lista libre desde el inicio. El primer
espacio suficientemente grande se utiliza; si sobra espacio, el bloque se
divide. Si ninguno sirve, los datos se agregan al final.

Si el objeto ya existe:

- Con el mismo tamaño se sobrescribe en su posición actual.
- Con diferente tamaño se libera el bloque anterior y la nueva versión se
  escribe al final.

Después se actualizan los metadatos y se utiliza `fsync` para solicitar que los
cambios lleguen al disco.

## Eliminación

Al eliminar un objeto, su región se agrega a la lista libre. La lista se ordena
por posición y los espacios contiguos se unen. Si el último espacio libre llega
hasta el final del archivo, el bucket se reduce con `ftruncate`.

## Funciones principales

- `storage_make_bucket`: crea el archivo del bucket.
- `storage_list_buckets`: busca archivos `.s3b`.
- `storage_list_objects`: consulta objetos y prefijos.
- `storage_put_object`: guarda o reemplaza un objeto.
- `storage_open_reader`: localiza un objeto para descargarlo.
- `storage_copy_object`: copia entre buckets o claves.
- `storage_move_object`: copia y luego elimina el origen.
- `storage_delete_object`: elimina objetos o prefijos.
- `storage_remove_bucket`: elimina un bucket vacío o lo fuerza.

## Aspectos útiles para las pruebas

- Un bucket recién creado debe ocupar exactamente 1 MiB.
- Los datos deben sobrevivir al reinicio del servidor.
- El tamaño del `.s3b` permite observar si se reutilizó un espacio.
- Descargar y comparar con `cmp` permite comprobar que no hubo corrupción.
- Un bucket con objetos debe rechazar `rb` si no se usa `--force`.
