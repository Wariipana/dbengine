# FSST

Copia de la implementación de referencia de FSST (https://github.com/cwida/fsst),
MIT, ver `LICENSE`. Sin modificar.

Se usa para comprimir **el contenido del diccionario** de un segmento
`kEncDictFsst`, no los valores fila a fila: el diccionario se descomprime una
vez al abrir el segmento y de ahí en más el acceso es el mismo que en
`kEncDict`.

Medido sobre el diccionario real de ClickBench: 1,96× en `Title`, 1,80× en
`URL`, 1,58× en `Referer`, descomprimiendo a 1.200–1.600 MB/s. Un scan
completo de las tres columnas cuesta 0,53 s de descompresión contra 7,8 s de
I/O que ahorra en frío.
