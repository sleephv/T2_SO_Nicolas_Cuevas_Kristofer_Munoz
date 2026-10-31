# Tarea 2 - Torneo de Komikan Concurrente (Sistemas Operativos - UDP)

**Integrantes:**
- Nicolas Cuevas
- Kristofer Muñoz

---

## 1. Descripción del Proyecto y Funciones Implementadas

Este proyecto implementa la infraestructura completa en C++17 para el **Torneo Multijugador de Komikan Concurrente en Tiempo Real**. A diferencia de los juegos por turnos tradicionales, en esta implementación el Puma y los 12 Perros operan como hilos POSIX (`pthread_t`) autónomos que compiten simultáneamente en intervalos de milisegundos por reclamar las 28 intersecciones del tablero tradicional mapuche.

### Componentes y Requisitos Cumplidos
- **Geometría Exacta del Tablero (`TableroKomikan`)**: Modelado como un grafo no dirigido de **28 nodos** compuesto por la cuadrícula principal de $5 \times 5$ (nodos `3` al `27`) acoplada en su extremo superior al triángulo o Trono (`T` en el nodo `0`, e intersecciones intermedias `1` y `2`), precalculando todas las adyacencias simples y las líneas rectas de 3 nodos para los saltos de captura del Puma.
- **Servidor Central y Matchmaker (`ServidorMatchmaker`)**: Recibe la llegada concurrente de $N$ clientes (`hilo_cliente`), los agrupa de a pares (1 Jugador Puma vs. 1 Jugador Perros) en una cola protegida por `mutex_mm` y maneja de forma pasiva el caso borde de $N$ impar mediante la variable de condición `cond_lobby`.
- **Control Estricto de $M$ Mesas Activas**: Utiliza el semáforo contador POSIX `sem_mesas_disponibles` inicializado en $M$, garantizando que jamás se supere el límite de $M$ partidas simultáneas y manteniendo a los pares sobrantes en espera pasiva (sin *busy-waiting*).
- **Juez Árbitro por Mesa (`hilo_juez`)**: Supervisa pasivamente cada partida mediante la variable de condición `cond_juez`, registra el estado visual del tablero y los eventos tanto en pantalla como en `torneo.log`, y decreta la victoria cuando el Puma captura 6 o más perros o cuando los Perros acorralan completamente al Puma.
- **Resiliencia ante Desconexiones/Fallos (Sección 4.1)**: Uso de manejadores de limpieza POSIX (`pthread_cleanup_push` / `pthread_cleanup_pop`) que liberan automáticamente el mutex retenido por un hilo si ocurre una anomalía o desconexión en plena partida, notificando al Juez para declarar ganador al oponente por abandono.
- **Clausura Ordenada ante `SIGINT` / `Ctrl+C` (Sección 4.2)**: Captura asíncrona mediante `sigaction`. Detiene inmediatamente la admisión de nuevos jugadores, notifica a todas las mesas en curso para que finalicen ordenadamente, recolecta todos los hilos con `pthread_join`, destruye todos los mutexes/semáforos/variables de condición y cierra los descriptores de archivos garantizando **cero fugas de memoria y cero recursos huérfanos**.

---

## 2. Modo de Uso (Compilación y Ejecución)

### Compilación Estricta
El proyecto incluye un `Makefile` configurado con las banderas obligatorias exigidas por el enunciado:
```bash
make
```
*(Comando ejecutado internamente: `g++ -Wall -Wextra -std=c++17 -pthread torneo.cpp -o torneo`)*

### Formato de Invocación
```bash
./torneo <N_Jugadores> <M_Mesas> [config.txt]
```

**Ejemplos de prueba:**
```bash
# Ejecución con 4 jugadores, máximo 2 mesas simultáneas y archivo de configuración
./torneo 4 2 config.txt

# Prueba de espera pasiva de mesas (6 jugadores = 3 partidas, pero solo 1 mesa física disponible)
./torneo 6 1 config.txt

# Prueba con número impar de clientes (5 jugadores: 2 partidas y 1 cliente impar retirado limpiamente)
./torneo 5 2 config.txt
```

### Parámetros del archivo `config.txt`
```ini
min_delay=50
max_delay=300
seed=1234
puma=0
perros=15,16,17,18,19,20,21,22,23,24,25,26
prob_fallo=2
```
*(Nota: `prob_fallo` define la probabilidad por mil en cada ciclo de movimiento de simular una desconexión/fallo interno para verificar la resiliencia de la Sección 4.1; usar `prob_fallo=0` para desactivar fallos simulados).*

### Limpieza de Binarios
```bash
make clean
```

---

## 3. Justificación de Decisiones de Diseño (Fundamentos de Sistemas Operativos)

1. **Patrón Monitor en cada Mesa (`mutex_tablero` y `cond_juez`)**:
   Cada instancia de `TableroKomikan` encapsula su estado compartido junto con un único mutex (`mutex_tablero`) y una variable de condición (`cond_juez`). Cuando el hilo del Puma o cualquiera de los 12 hilos de los Perros concluye su ciclo de decisión (`usleep` aleatorio entre `50` y `300 ms`), adquiere `mutex_tablero`, verifica atómicamente si la casilla destino (o el salto de captura) sigue disponible, actualiza el tablero y despierta al Juez con `pthread_cond_signal` antes de liberar el cerrojo.
2. **Eliminación por Diseño del Riesgo de Deadlock**:
   Al centralizar la validación y actualización de cada movimiento en una transacción atómica corta bajo `mutex_tablero` (evitando la retención anidada de múltiples cerrojos simultáneos sobre casillas distintas), se rompe estructuralmente la condición de *Hold and Wait* (Retención y Espera) y de *Espera Circular* de Coffman, haciendo imposible que dos fichas que intentan cruzar posiciones entren en interbloqueo (*Deadlock*).
3. **Mitigación de Starvation y Livelock**:
   Cada hilo utiliza su propia semilla local independiente con la función reentrante `rand_r(&seed_local)` y libera el procesador mediante `usleep` fuera de la sección crítica. Dado que la sección crítica toma apenas microsegundos frente a los `50-300 ms` de retardo entre movimientos, la contención sobre `mutex_tablero` es mínima y el planificador del kernel Linux (CFS) otorga acceso equitativo a las 13 fichas.
4. **Espera Pasiva (Zero Busy-Waiting)**:
   Tanto los clientes impares en el lobby (`pthread_cond_wait` sobre `cond_lobby`), como las partidas que aguardan una mesa libre (`sem_wait` sobre `sem_mesas_disponibles`) y el Juez Árbitro (`pthread_cond_wait` sobre `cond_juez`), bloquean su ejecución a nivel de kernel cediendo la CPU hasta que ocurre un evento real.
El sistema consta de:
- Un **Servidor (Matchmaker)** que empareja jugadores y asigna mesas.
- **Mesas de Juego Concurrentes** gestionadas pasivamente mediante semáforos.
- Un **Juez Árbitro** por mesa que valida movimientos, saltos de captura y declara la victoria.

## 2. Restricciones Técnicas y Compilación
- **Lenguaje:** C++ (Estándar C++17)
- **Sincronización:** Uso exclusivo de primitivas POSIX (`pthread_mutex`, `pthread_cond`, `sem_t`).

### Compilación Estricta
Se provee un `Makefile` para automatizar la compilación con los flags exigidos:
```bash
make
```
*(Comando interno ejecutado: `g++ -Wall -Wextra -std=c++17 -pthread torneo.cpp -o torneo`)*

### Modo de Uso
El programa se invoca pasando el número de jugadores ($N$), el límite de mesas ($M$) y, opcionalmente, un archivo de configuración:
```bash
./torneo <N> <M> [config.txt]
```
**Ejemplo de ejecución:**
```bash
./torneo 4 2 config.txt
```

### Limpieza de Binarios
```bash
make clean
```

---

##  3. Resumen de Diseño y Sincronización (Extracto)
- **Geometría del Tablero:** Modelado como un grafo de 28 nodos (25 de la grilla + 3 del Trono) para representar fielmente las adyacencias.
- **Prevención de Race Conditions:** Se utiliza un patrón de Monitor con un *mutex general de mesa* para arbitrar las peticiones de movimiento y *mutexes individuales por casilla* para asegurar la exclusión mutua atómica al momento de ocupar un espacio físico en el grafo.
- **Gestión de Recursos (Mesas):** Se implementó un control de acceso para garantizar que nunca se exceda el límite de $M$ mesas activas simultáneamente, manteniendo a los jugadores sobrantes en espera pasiva (sin *busy-waiting*).
- **Manejo de Señales (Resiliencia):** Captura de `SIGINT` (Ctrl+C) para abortar limpiamente las partidas activas, destruyendo los mutexes/semáforos y garantizando cero fugas de memoria.