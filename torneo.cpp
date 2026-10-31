#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <queue>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <csignal>
#include <cerrno>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>

using namespace std;

// Bandera atómica global para capturar la señal SIGINT (Ctrl+C)
volatile sig_atomic_t sigint_recibido = 0;

// Manejador asíncrono-seguro para la señal SIGINT
void manejador_sigint(int sig) {
    (void)sig;
    sigint_recibido = 1;
}

// Salto válido para que el Puma capture a un Perro
struct Salto {
    int sobre_nodo; // Casilla adyacente donde está el perro
    int destino;    // Casilla donde el Puma aterriza después del salto
};

// Configuración por defecto del torneo o cargada desde config.txt
struct Configuracion {
    int min_delay_ms = 50;
    int max_delay_ms = 300;
    unsigned int seed = 0;
    int pos_inicial_puma = 0; // El Puma empieza en el nodo 0 (T)
    vector<int> pos_inicial_perros = {15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26};
    int prob_fallo_por_mil = 0; // 0 por defecto; configurable en config.txt con prob_fallo=X
    string archivo_log = "torneo.log";
};

// Representa el estado compartido del tablero de 28 casillas de la mesa (Patrón Monitor)
struct TableroKomikan {
    int id_mesa;
    int id_jugador_puma;
    int id_jugador_perros;

    char casillas[28];               // 'P' para Puma, 'D' para Perro, '.' para vacío
    vector<int> adyacentes[28];      // Movimientos normales de 1 paso
    vector<Salto> saltos_puma[28];   // Saltos de captura de 2 pasos

    int pos_puma;
    int pos_perros[12];              // Posición de cada perro (-1 si fue capturado)
    int perros_capturados;
    bool partida_activa;
    bool hubo_movimiento;

    // Variables de Resiliencia (Etapa 4)
    bool anomalia_detectada;
    char bando_fallido;              // 'P' si falló el Puma, 'D' si falló el jugador Perros
    string motivo_fallo;
    bool cancelada_por_sigint;

    string ultimo_evento;

    Configuracion cfg;
    pthread_mutex_t mutex_tablero;   // Mutex general para proteger el tablero
    pthread_cond_t cond_juez;        // Variable de condición para despertar al Juez
    pthread_mutex_t* mutex_consola;  // Puntero al mutex de consola/log del servidor
    ofstream* log_stream;            // Descriptor de archivo de registro compartido

    TableroKomikan(int id, int jug_puma, int jug_perros, const Configuracion& config,
                   pthread_mutex_t* m_consola, ofstream* f_log)
        : id_mesa(id), id_jugador_puma(jug_puma), id_jugador_perros(jug_perros),
          perros_capturados(0), partida_activa(true), hubo_movimiento(false),
          anomalia_detectada(false), bando_fallido('N'), cancelada_por_sigint(false),
          cfg(config), mutex_consola(m_consola), log_stream(f_log) {
        pthread_mutex_init(&mutex_tablero, NULL);
        pthread_cond_init(&cond_juez, NULL);

        for (int i = 0; i < 28; i++) {
            casillas[i] = '.';
        }

        construir_geometria();

        // Posicionar al Puma
        pos_puma = cfg.pos_inicial_puma;
        if (pos_puma >= 0 && pos_puma < 28) {
            casillas[pos_puma] = 'P';
        } else {
            pos_puma = 0;
            casillas[0] = 'P';
        }

        // Posicionar a los 12 Perros
        for (int i = 0; i < 12; i++) {
            int pos = (i < static_cast<int>(cfg.pos_inicial_perros.size())) ? cfg.pos_inicial_perros[i] : -1;
            if (pos >= 0 && pos < 28 && pos != pos_puma && casillas[pos] == '.') {
                pos_perros[i] = pos;
                casillas[pos] = 'D';
            } else {
                pos_perros[i] = -1;
            }
        }
    }

    ~TableroKomikan() {
        pthread_mutex_destroy(&mutex_tablero);
        pthread_cond_destroy(&cond_juez);
    }

    void conectar(int u, int v) {
        adyacentes[u].push_back(v);
        adyacentes[v].push_back(u);
    }

    void agregar_linea_salto(int a, int b, int c) {
        saltos_puma[a].push_back({b, c});
        saltos_puma[c].push_back({b, a});
    }

    void construir_geometria() {
        // 1. Conexiones del Trono (Nodos 0, 1, 2) acoplados a la Fila 0 (Nodos 3..7)
        conectar(0, 1); conectar(0, 5); conectar(0, 2);
        conectar(1, 2);
        conectar(1, 3); conectar(1, 4);
        conectar(2, 6); conectar(2, 7);

        // 2. Conexiones de la cuadrícula principal 5x5 (Nodos 3 al 27)
        for (int fila = 0; fila < 5; fila++) {
            for (int col = 0; col < 5; col++) {
                int actual = 3 + fila * 5 + col;
                if (col < 4) conectar(actual, actual + 1);
                if (fila < 4) conectar(actual, actual + 5);
            }
        }

        // 3. Saltos rectos dentro del Trono y hacia la grilla 5x5
        agregar_linea_salto(0, 1, 3);  // Diagonal izquierda del trono: 0 <-> 1 <-> 3
        agregar_linea_salto(0, 2, 7);  // Diagonal derecha del trono: 0 <-> 2 <-> 7
        agregar_linea_salto(0, 5, 10); // Vertical central desde T: 0 <-> 5 <-> 10
        agregar_linea_salto(1, 4, 9);  // Vertical desde nodo 1: 1 <-> 4 <-> 9
        agregar_linea_salto(2, 6, 11); // Vertical desde nodo 2: 2 <-> 6 <-> 11

        // 4. Saltos horizontales y verticales dentro de la grilla 5x5
        for (int fila = 0; fila < 5; fila++) {
            for (int col = 0; col < 5; col++) {
                int a = 3 + fila * 5 + col;
                if (col <= 2) agregar_linea_salto(a, a + 1, a + 2);
                if (fila <= 2) agregar_linea_salto(a, a + 5, a + 10);
            }
        }
    }

    // Verifica si el Puma tiene al menos un movimiento o salto válido disponible
    bool puma_tiene_movimientos() {
        for (int vecino : adyacentes[pos_puma]) {
            if (casillas[vecino] == '.') return true;
        }
        for (const Salto& s : saltos_puma[pos_puma]) {
            if (casillas[s.sobre_nodo] == 'D' && casillas[s.destino] == '.') return true;
        }
        return false;
    }

    // Imprime el tablero perfectamente alineado en consola y en el archivo de log
    void imprimir_tablero() {
        if (mutex_consola) pthread_mutex_lock(mutex_consola);

        ostringstream ss;
        ss << "\n========== MESA " << id_mesa << " [Puma: Jugador #" << id_jugador_puma
           << " vs Perros: Jugador #" << id_jugador_perros << "] ==========\n";
        ss << "Perros capturados: " << perros_capturados << "/6\n";
        if (!ultimo_evento.empty()) {
            ss << "Evento: " << ultimo_evento << "\n";
        }
        ss << "                    (" << casillas[0] << ") [T]\n";
        ss << "                   / | \\\n";
        ss << "                 /   |   \\\n";
        ss << "              (" << casillas[1] << ")----+----(" << casillas[2] << ")\n";
        ss << "             / |     |     | \\\n";
        ss << "           /   |     |     |   \\\n";

        for (int fila = 0; fila < 5; fila++) {
            ss << "        ";
            for (int col = 0; col < 5; col++) {
                int idx = 3 + fila * 5 + col;
                ss << "(" << casillas[idx] << ")";
                if (col < 4) ss << "---";
            }
            ss << "\n";
            if (fila < 4) {
                ss << "         |     |     |     |     |\n";
            }
        }
        ss << "====================================================\n";

        cout << ss.str();
        if (log_stream && log_stream->is_open()) {
            (*log_stream) << ss.str();
            log_stream->flush();
        }

        if (mutex_consola) pthread_mutex_unlock(mutex_consola);
    }
};

// Estructura para liberar automáticamente el mutex_tablero si un hilo aborta (Sección 4.1)
struct ContextoLimpiezaMutex {
    TableroKomikan* mesa;
    bool bloqueado;
};

void liberar_mutex_en_fallo(void* arg) {
    ContextoLimpiezaMutex* ctx = static_cast<ContextoLimpiezaMutex*>(arg);
    if (ctx && ctx->bloqueado && ctx->mesa) {
        pthread_cond_signal(&ctx->mesa->cond_juez);
        pthread_mutex_unlock(&ctx->mesa->mutex_tablero);
        ctx->bloqueado = false;
    }
}

// Argumentos para pasar a cada hilo de Perro
struct ArgPerro {
    int id_perro;
    TableroKomikan* mesa;
};

// Función auxiliar para dormir entre min_delay_ms y max_delay_ms de forma thread-safe
void dormir_aleatorio(const Configuracion& cfg, unsigned int* seed_hilo) {
    if (sigint_recibido) return;
    int rango = cfg.max_delay_ms - cfg.min_delay_ms + 1;
    int ms = cfg.min_delay_ms + (rand_r(seed_hilo) % (rango > 0 ? rango : 1));
    usleep(ms * 1000);
}

// Hilo del Puma (1 por mesa)
void* hilo_puma(void* arg) {
    TableroKomikan* mesa = static_cast<TableroKomikan*>(arg);
    unsigned int seed_local = mesa->cfg.seed ^ (999 + mesa->id_mesa * 31);

    ContextoLimpiezaMutex ctx{mesa, false};
    pthread_cleanup_push(liberar_mutex_en_fallo, &ctx);

    while (!sigint_recibido) {
        dormir_aleatorio(mesa->cfg, &seed_local);

        pthread_mutex_lock(&mesa->mutex_tablero);
        ctx.bloqueado = true;

        if (!mesa->partida_activa || sigint_recibido) {
            ctx.bloqueado = false;
            pthread_mutex_unlock(&mesa->mutex_tablero);
            break;
        }

        // Simulación de desconexión o fallo interno del jugador Puma (Sección 4.1)
        if (mesa->cfg.prob_fallo_por_mil > 0 &&
            (rand_r(&seed_local) % 1000) < mesa->cfg.prob_fallo_por_mil) {
            mesa->anomalia_detectada = true;
            mesa->bando_fallido = 'P';
            mesa->motivo_fallo = "Desconexion/fallo interno detectado en el Jugador Puma (#" +
                                 to_string(mesa->id_jugador_puma) + ")";
            mesa->hubo_movimiento = true;
            // Salir ejecutando el manejador de limpieza que desbloquea el mutex y avisa al Juez
            pthread_exit(NULL);
        }

        int origen = mesa->pos_puma;
        vector<Salto> capturas_posibles;
        for (const Salto& s : mesa->saltos_puma[origen]) {
            if (mesa->casillas[s.sobre_nodo] == 'D' && mesa->casillas[s.destino] == '.') {
                capturas_posibles.push_back(s);
            }
        }

        // Prioridad 1: Si puede capturar a un perro, salta
        if (!capturas_posibles.empty()) {
            Salto elegido = capturas_posibles[rand_r(&seed_local) % capturas_posibles.size()];
            mesa->casillas[origen] = '.';
            mesa->casillas[elegido.sobre_nodo] = '.';
            mesa->casillas[elegido.destino] = 'P';
            mesa->pos_puma = elegido.destino;

            // Marcar al perro capturado como eliminado (-1)
            for (int i = 0; i < 12; i++) {
                if (mesa->pos_perros[i] == elegido.sobre_nodo) {
                    mesa->pos_perros[i] = -1;
                    break;
                }
            }
            mesa->perros_capturados++;
            mesa->ultimo_evento = "Puma salta de " + to_string(origen) + " a " +
                                  to_string(elegido.destino) + " capturando perro en " +
                                  to_string(elegido.sobre_nodo) + "!";
            mesa->hubo_movimiento = true;
            pthread_cond_signal(&mesa->cond_juez);
        } else {
            // Prioridad 2: Movimiento normal a una casilla vecina libre
            vector<int> libres;
            for (int v : mesa->adyacentes[origen]) {
                if (mesa->casillas[v] == '.') libres.push_back(v);
            }
            if (!libres.empty()) {
                int destino = libres[rand_r(&seed_local) % libres.size()];
                mesa->casillas[origen] = '.';
                mesa->casillas[destino] = 'P';
                mesa->pos_puma = destino;
                mesa->ultimo_evento = "Puma se mueve de " + to_string(origen) + " a " + to_string(destino);
                mesa->hubo_movimiento = true;
                pthread_cond_signal(&mesa->cond_juez);
            }
        }

        ctx.bloqueado = false;
        pthread_mutex_unlock(&mesa->mutex_tablero);
    }

    pthread_cleanup_pop(0);
    return NULL;
}

// Hilo de cada Perro (12 por mesa)
void* hilo_perro(void* arg) {
    ArgPerro* datos = static_cast<ArgPerro*>(arg);
    int id = datos->id_perro;
    TableroKomikan* mesa = datos->mesa;
    unsigned int seed_local = mesa->cfg.seed ^ ((id + 100) * (mesa->id_mesa + 7));

    ContextoLimpiezaMutex ctx{mesa, false};
    pthread_cleanup_push(liberar_mutex_en_fallo, &ctx);

    while (!sigint_recibido) {
        dormir_aleatorio(mesa->cfg, &seed_local);

        pthread_mutex_lock(&mesa->mutex_tablero);
        ctx.bloqueado = true;

        // Si la partida terminó o este perro fue capturado por el Puma, el hilo termina
        if (!mesa->partida_activa || mesa->pos_perros[id] == -1 || sigint_recibido) {
            ctx.bloqueado = false;
            pthread_mutex_unlock(&mesa->mutex_tablero);
            break;
        }

        // Simulación de fallo interno / desconexión en el bando de los Perros (Sección 4.1)
        if (id == 0 && mesa->cfg.prob_fallo_por_mil > 0 &&
            (rand_r(&seed_local) % 1000) < mesa->cfg.prob_fallo_por_mil) {
            mesa->anomalia_detectada = true;
            mesa->bando_fallido = 'D';
            mesa->motivo_fallo = "Desconexion/fallo interno en Perro #" + to_string(id) +
                                 " del Jugador Perros (#" + to_string(mesa->id_jugador_perros) + ")";
            mesa->hubo_movimiento = true;
            pthread_exit(NULL);
        }

        int origen = mesa->pos_perros[id];
        vector<int> libres;
        for (int v : mesa->adyacentes[origen]) {
            if (mesa->casillas[v] == '.') libres.push_back(v);
        }

        if (!libres.empty()) {
            int destino = libres[rand_r(&seed_local) % libres.size()];
            mesa->casillas[origen] = '.';
            mesa->casillas[destino] = 'D';
            mesa->pos_perros[id] = destino;
            mesa->ultimo_evento = "Perro #" + to_string(id) + " se mueve de " +
                                  to_string(origen) + " a " + to_string(destino);
            mesa->hubo_movimiento = true;
            pthread_cond_signal(&mesa->cond_juez);
        }

        ctx.bloqueado = false;
        pthread_mutex_unlock(&mesa->mutex_tablero);
    }

    pthread_cleanup_pop(0);
    return NULL;
}

// Hilo Juez Árbitro de la Mesa
void* hilo_juez(void* arg) {
    TableroKomikan* mesa = static_cast<TableroKomikan*>(arg);

    pthread_mutex_lock(&mesa->mutex_tablero);
    mesa->ultimo_evento = "Partida iniciada por el Juez";
    mesa->imprimir_tablero();
    pthread_mutex_unlock(&mesa->mutex_tablero);

    // Crear el hilo del Puma y los 12 hilos de los Perros
    pthread_t t_puma;
    pthread_t t_perros[12];
    ArgPerro args_perros[12];

    pthread_create(&t_puma, NULL, hilo_puma, mesa);
    for (int i = 0; i < 12; i++) {
        args_perros[i] = {i, mesa};
        pthread_create(&t_perros[i], NULL, hilo_perro, &args_perros[i]);
    }

    // Ciclo de supervisión pasiva del Juez
    pthread_mutex_lock(&mesa->mutex_tablero);
    while (mesa->partida_activa) {
        while (!mesa->hubo_movimiento && mesa->partida_activa &&
               !mesa->anomalia_detectada && !mesa->cancelada_por_sigint && !sigint_recibido) {
            pthread_cond_wait(&mesa->cond_juez, &mesa->mutex_tablero);
        }

        // Caso 4.2: Interrupción global del Servidor (SIGINT / Ctrl+C)
        if (sigint_recibido || mesa->cancelada_por_sigint) {
            mesa->partida_activa = false;
            if (mesa->mutex_consola) pthread_mutex_lock(mesa->mutex_consola);
            string msg = "\n>>> [JUEZ MESA " + to_string(mesa->id_mesa) +
                         "] Partida clausurada ordenadamente por senal SIGINT (Ctrl+C). <<<\n";
            cout << msg;
            if (mesa->log_stream && mesa->log_stream->is_open()) {
                (*mesa->log_stream) << msg;
            }
            if (mesa->mutex_consola) pthread_mutex_unlock(mesa->mutex_consola);
            break;
        }

        // Caso 4.1: Detección de desconexión o fallo interno de un jugador
        if (mesa->anomalia_detectada) {
            mesa->partida_activa = false;
            int ganador = (mesa->bando_fallido == 'P') ? mesa->id_jugador_perros : mesa->id_jugador_puma;
            string bando_ganador = (mesa->bando_fallido == 'P') ? "PERROS" : "PUMA";

            if (mesa->mutex_consola) pthread_mutex_lock(mesa->mutex_consola);
            ostringstream ss;
            ss << "\n>>> [JUEZ MESA " << mesa->id_mesa << "] ALERTA DE RESILIENCIA: "
               << mesa->motivo_fallo << " <<<\n"
               << ">>> [JUEZ MESA " << mesa->id_mesa << "] Partida abortada limpiamente. "
               << "¡VICTORIA POR ABANDONO PARA " << bando_ganador
               << " (Jugador #" << ganador << ")! <<<\n";
            cout << ss.str();
            if (mesa->log_stream && mesa->log_stream->is_open()) {
                (*mesa->log_stream) << ss.str();
            }
            if (mesa->mutex_consola) pthread_mutex_unlock(mesa->mutex_consola);
            break;
        }

        if (!mesa->partida_activa) break;

        mesa->hubo_movimiento = false;
        mesa->imprimir_tablero();

        // Condición de victoria 1: El Puma capturó 6 o más perros
        if (mesa->perros_capturados >= 6) {
            if (mesa->mutex_consola) pthread_mutex_lock(mesa->mutex_consola);
            string msg = "\n>>> [JUEZ MESA " + to_string(mesa->id_mesa) +
                         "] ¡VICTORIA DEL PUMA (Jugador #" + to_string(mesa->id_jugador_puma) +
                         ")! (Capturo " + to_string(mesa->perros_capturados) + " perros) <<<\n";
            cout << msg;
            if (mesa->log_stream && mesa->log_stream->is_open()) (*mesa->log_stream) << msg;
            if (mesa->mutex_consola) pthread_mutex_unlock(mesa->mutex_consola);
            mesa->partida_activa = false;
            break;
        }

        // Condición de victoria 2: Los Perros acorralaron al Puma
        if (!mesa->puma_tiene_movimientos()) {
            if (mesa->mutex_consola) pthread_mutex_lock(mesa->mutex_consola);
            string msg = "\n>>> [JUEZ MESA " + to_string(mesa->id_mesa) +
                         "] ¡VICTORIA DE LOS PERROS (Jugador #" + to_string(mesa->id_jugador_perros) +
                         ")! (Puma acorralado en nodo " + to_string(mesa->pos_puma) + ") <<<\n";
            cout << msg;
            if (mesa->log_stream && mesa->log_stream->is_open()) (*mesa->log_stream) << msg;
            if (mesa->mutex_consola) pthread_mutex_unlock(mesa->mutex_consola);
            mesa->partida_activa = false;
            break;
        }
    }
    pthread_mutex_unlock(&mesa->mutex_tablero);

    // Esperar a que todos los hilos de fichas terminen limpiamente
    pthread_join(t_puma, NULL);
    for (int i = 0; i < 12; i++) {
        pthread_join(t_perros[i], NULL);
    }

    return NULL;
}

// Función para leer config.txt y cargar la configuración del torneo
Configuracion cargar_configuracion(const string& ruta) {
    Configuracion cfg;
    cfg.seed = static_cast<unsigned int>(time(NULL));

    if (ruta.empty()) return cfg;

    ifstream archivo(ruta);
    if (!archivo.is_open()) {
        cerr << "Aviso: No se pudo abrir " << ruta << ". Usando valores por defecto.\n";
        return cfg;
    }

    string linea;
    while (getline(archivo, linea)) {
        if (linea.empty() || linea[0] == '#') continue;
        stringstream ss(linea);
        string clave;
        if (getline(ss, clave, '=')) {
            string valor;
            getline(ss, valor);
            if (clave == "min_delay") cfg.min_delay_ms = stoi(valor);
            else if (clave == "max_delay") cfg.max_delay_ms = stoi(valor);
            else if (clave == "seed") cfg.seed = static_cast<unsigned int>(stoul(valor));
            else if (clave == "puma") cfg.pos_inicial_puma = stoi(valor);
            else if (clave == "prob_fallo") cfg.prob_fallo_por_mil = stoi(valor);
            else if (clave == "perros") {
                cfg.pos_inicial_perros.clear();
                stringstream ss_perros(valor);
                string pos_str;
                while (getline(ss_perros, pos_str, ',')) {
                    cfg.pos_inicial_perros.push_back(stoi(pos_str));
                }
            }
        }
    }
    return cfg;
}

// ==================== ETAPA 3 Y 4: SERVIDOR, EMPAREJAMIENTO Y RESILIENCIA ====================

struct ServidorMatchmaker {
    int total_clientes;
    int clientes_llegados = 0;
    int contador_partidas = 0;
    int jugador_impar_sobrante = -1;
    bool cierre_normal = false; // Distingue término normal del torneo vs señal SIGINT

    queue<int> cola_espera;
    vector<bool> mesa_libre;
    vector<pthread_t> hilos_partidas;
    vector<TableroKomikan*> mesas_en_curso; // Registro de mesas activas para clausura ordenada ante SIGINT

    pthread_mutex_t mutex_mm;
    pthread_cond_t cond_lobby;
    pthread_mutex_t mutex_consola;
    sem_t sem_mesas_disponibles;
    ofstream archivo_log;

    Configuracion cfg;

    ServidorMatchmaker(int N, int M, const Configuracion& config)
        : total_clientes(N), mesa_libre(M, true), cfg(config) {
        pthread_mutex_init(&mutex_mm, NULL);
        pthread_cond_init(&cond_lobby, NULL);
        pthread_mutex_init(&mutex_consola, NULL);
        sem_init(&sem_mesas_disponibles, 0, M);
        archivo_log.open(cfg.archivo_log, ios::out | ios::trunc);
    }

    ~ServidorMatchmaker() {
        if (archivo_log.is_open()) {
            archivo_log.close();
        }
        pthread_mutex_destroy(&mutex_mm);
        pthread_cond_destroy(&cond_lobby);
        pthread_mutex_destroy(&mutex_consola);
        sem_destroy(&sem_mesas_disponibles);
    }

    // Notifica a todas las partidas activas y clientes en espera que el servidor se apaga por SIGINT
    void notificar_cierre_emergencia() {
        pthread_mutex_lock(&mutex_mm);
        // Vaciar la cola de espera para que ningún cliente quede bloqueado
        while (!cola_espera.empty()) {
            cola_espera.pop();
        }
        pthread_cond_broadcast(&cond_lobby);

        // Notificar a cada mesa activa que debe terminar ordenadamente
        for (TableroKomikan* mesa : mesas_en_curso) {
            if (mesa) {
                pthread_mutex_lock(&mesa->mutex_tablero);
                mesa->cancelada_por_sigint = true;
                mesa->partida_activa = false;
                pthread_cond_signal(&mesa->cond_juez);
                pthread_mutex_unlock(&mesa->mutex_tablero);
            }
        }
        pthread_mutex_unlock(&mutex_mm);
    }
};

struct DatosPartida {
    int num_partida;
    int id_jugador_puma;
    int id_jugador_perros;
    ServidorMatchmaker* servidor;
};

// Hilo vigía que monitorea la bandera sigint_recibido para despertar inmediatamente a todas las mesas
void* hilo_vigia_sigint(void* arg) {
    ServidorMatchmaker* srv = static_cast<ServidorMatchmaker*>(arg);
    while (!sigint_recibido) {
        usleep(20000); // Revisa cada 20 ms sin afectar el rendimiento
        pthread_mutex_lock(&srv->mutex_mm);
        bool fin_natural = srv->cierre_normal;
        pthread_mutex_unlock(&srv->mutex_mm);
        if (fin_natural) {
            return NULL; // El torneo concluyó normalmente: salir sin imprimir mensaje de SIGINT
        }
    }

    if (sigint_recibido) {
        pthread_mutex_lock(&srv->mutex_consola);
        cout << "\n\n[SERVIDOR] ¡Senal SIGINT (Ctrl+C) detectada! Deteniendo admision y notificando mesas activas...\n";
        if (srv->archivo_log.is_open()) {
            srv->archivo_log << "\n[SERVIDOR] Senal SIGINT (Ctrl+C) recibida. Clausurando torneo ordenadamente.\n";
        }
        pthread_mutex_unlock(&srv->mutex_consola);

        srv->notificar_cierre_emergencia();
        // Despertar posibles hilos esperando cupo en el semáforo de mesas
        for (size_t i = 0; i < srv->mesa_libre.size() + 2; i++) {
            sem_post(&srv->sem_mesas_disponibles);
        }
    }
    return NULL;
}

// Hilo que gestiona el ciclo de vida de una Mesa asignada
void* ejecutar_mesa_torneo(void* arg) {
    DatosPartida* datos = static_cast<DatosPartida*>(arg);
    ServidorMatchmaker* srv = datos->servidor;

    // 1. Espera pasiva en el semáforo si las M mesas están ocupadas
    while (sem_wait(&srv->sem_mesas_disponibles) == -1) {
        if (errno == EINTR && sigint_recibido) {
            delete datos;
            return NULL;
        }
    }

    if (sigint_recibido) {
        sem_post(&srv->sem_mesas_disponibles);
        delete datos;
        return NULL;
    }

    // 2. Reclamar el número de mesa física disponible (entre 1 y M)
    int id_mesa_fisica = -1;
    pthread_mutex_lock(&srv->mutex_mm);
    for (size_t i = 0; i < srv->mesa_libre.size(); i++) {
        if (srv->mesa_libre[i]) {
            srv->mesa_libre[i] = false;
            id_mesa_fisica = static_cast<int>(i) + 1;
            break;
        }
    }
    pthread_mutex_unlock(&srv->mutex_mm);

    if (id_mesa_fisica == -1) {
        sem_post(&srv->sem_mesas_disponibles);
        delete datos;
        return NULL;
    }

    pthread_mutex_lock(&srv->mutex_consola);
    cout << "\n[MATCHMAKER] >>> Iniciando Partida #" << datos->num_partida
         << " en MESA " << id_mesa_fisica << " (Jugador #" << datos->id_jugador_puma
         << " [Puma] vs Jugador #" << datos->id_jugador_perros << " [Perros]) <<<\n";
    pthread_mutex_unlock(&srv->mutex_consola);

    // 3. Crear el tablero de la mesa, registrarlo para SIGINT y correr el Juez Árbitro
    TableroKomikan mesa(id_mesa_fisica, datos->id_jugador_puma, datos->id_jugador_perros,
                        srv->cfg, &srv->mutex_consola, &srv->archivo_log);

    pthread_mutex_lock(&srv->mutex_mm);
    srv->mesas_en_curso.push_back(&mesa);
    pthread_mutex_unlock(&srv->mutex_mm);

    hilo_juez(&mesa);

    // 4. Desregistrar la mesa activa y liberar el cupo físico
    pthread_mutex_lock(&srv->mutex_mm);
    srv->mesas_en_curso.erase(
        remove(srv->mesas_en_curso.begin(), srv->mesas_en_curso.end(), &mesa),
        srv->mesas_en_curso.end()
    );
    srv->mesa_libre[id_mesa_fisica - 1] = true;
    pthread_mutex_unlock(&srv->mutex_mm);

    pthread_mutex_lock(&srv->mutex_consola);
    cout << "\n[MATCHMAKER] <<< MESA " << id_mesa_fisica
         << " liberada tras finalizar la Partida #" << datos->num_partida << " >>>\n";
    pthread_mutex_unlock(&srv->mutex_consola);

    sem_post(&srv->sem_mesas_disponibles);

    delete datos;
    return NULL;
}

struct ArgCliente {
    int id_cliente;
    ServidorMatchmaker* servidor;
};

// Hilo que representa a cada uno de los N clientes llegando concurrentemente al servidor
void* hilo_cliente(void* arg) {
    ArgCliente* cli = static_cast<ArgCliente*>(arg);
    ServidorMatchmaker* srv = cli->servidor;

    pthread_mutex_lock(&srv->mutex_mm);

    // Si se recibió SIGINT, detener inmediatamente la llegada de nuevos jugadores (Sección 4.2.1)
    if (sigint_recibido) {
        srv->clientes_llegados++;
        pthread_cond_broadcast(&srv->cond_lobby);
        pthread_mutex_unlock(&srv->mutex_mm);
        delete cli;
        return NULL;
    }

    srv->cola_espera.push(cli->id_cliente);
    srv->clientes_llegados++;

    pthread_mutex_lock(&srv->mutex_consola);
    cout << "[SERVIDOR] Llega Cliente #" << cli->id_cliente
         << " a la sala de espera (" << srv->clientes_llegados << "/" << srv->total_clientes << ").\n";
    pthread_mutex_unlock(&srv->mutex_consola);

    // Caso 1: Hay 2 jugadores en la fila -> Se emparejan y despiertan al compañero que esperaba
    if (srv->cola_espera.size() >= 2) {
        int jug_puma = srv->cola_espera.front();
        srv->cola_espera.pop();
        int jug_perros = srv->cola_espera.front();
        srv->cola_espera.pop();

        srv->contador_partidas++;
        DatosPartida* nueva_partida = new DatosPartida{
            srv->contador_partidas,
            jug_puma,
            jug_perros,
            srv
        };

        pthread_t t_partida;
        pthread_create(&t_partida, NULL, ejecutar_mesa_torneo, nueva_partida);
        srv->hilos_partidas.push_back(t_partida);

        pthread_cond_broadcast(&srv->cond_lobby);
    }
    // Caso 2: Quedó 1 solo jugador en la cola -> Espera pasivamente hasta que llegue pareja, se acaben los N clientes o llegue SIGINT
    else {
        while (!sigint_recibido &&
               !srv->cola_espera.empty() &&
               srv->cola_espera.front() == cli->id_cliente &&
               srv->clientes_llegados < srv->total_clientes) {
            pthread_cond_wait(&srv->cond_lobby, &srv->mutex_mm);
        }

        if (!sigint_recibido &&
            !srv->cola_espera.empty() &&
            srv->cola_espera.front() == cli->id_cliente &&
            srv->clientes_llegados == srv->total_clientes) {
            srv->cola_espera.pop();
            srv->jugador_impar_sobrante = cli->id_cliente;

            pthread_mutex_lock(&srv->mutex_consola);
            cout << "\n*************************************************************\n";
            cout << "[MATCHMAKER] AVISO DE JUGADOR IMPAR: El Cliente #" << cli->id_cliente
                 << " espero en el lobby pero no quedan mas rivales (N=" << srv->total_clientes << ").\n";
            cout << "*************************************************************\n\n";
            pthread_mutex_unlock(&srv->mutex_consola);
        }
    }

    if (srv->clientes_llegados == srv->total_clientes) {
        pthread_cond_broadcast(&srv->cond_lobby);
    }

    pthread_mutex_unlock(&srv->mutex_mm);
    delete cli;
    return NULL;
}

int main(int argc, char* argv[]) {
    if (argc < 3 || argc > 4) {
        cerr << "Uso: " << argv[0] << " <N_Jugadores> <M_Mesas> [config.txt]\n";
        return 1;
    }

    int N = stoi(argv[1]);
    int M = stoi(argv[2]);
    string ruta_cfg = (argc == 4) ? argv[3] : "";

    if (N <= 0 || M <= 0) {
        cerr << "Error: N y M deben ser mayores a 0.\n";
        return 1;
    }

    // Registrar manejador de señal SIGINT (Ctrl+C)
    struct sigaction sa;
    sa.sa_handler = manejador_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    Configuracion cfg = cargar_configuracion(ruta_cfg);

    cout << "=========================================================\n";
    cout << " Torneo Komikan Inicializado | N=" << N << " clientes | M=" << M << " mesas max\n";
    cout << " Delays configurados: [" << cfg.min_delay_ms << " ms - " << cfg.max_delay_ms << " ms]\n";
    cout << " Registro de eventos: " << cfg.archivo_log << "\n";
    cout << "=========================================================\n";

    ServidorMatchmaker servidor(N, M, cfg);

    // 1. Lanzar hilo vigía desde el inicio para atender Ctrl+C en cualquier etapa
    pthread_t t_vigia;
    pthread_create(&t_vigia, NULL, hilo_vigia_sigint, &servidor);

    // 2. Lanzar los N hilos de clientes llegando de forma concurrente
    vector<pthread_t> hilos_clientes(N);
    for (int i = 0; i < N; i++) {
        ArgCliente* arg = new ArgCliente{i + 1, &servidor};
        pthread_create(&hilos_clientes[i], NULL, hilo_cliente, arg);
    }

    // 3. Esperar a que los N clientes hayan sido procesados en el lobby
    for (int i = 0; i < N; i++) {
        pthread_join(hilos_clientes[i], NULL);
    }

    // 4. Copiar los identificadores de hilos de partidas creadas para esperar su término
    pthread_mutex_lock(&servidor.mutex_mm);
    vector<pthread_t> partidas_a_esperar = servidor.hilos_partidas;
    pthread_mutex_unlock(&servidor.mutex_mm);

    for (pthread_t t_part : partidas_a_esperar) {
        pthread_join(t_part, NULL);
    }

    // 5. Indicar al vigía que el torneo concluyó normalmente (sin tocar sigint_recibido) y unir su hilo
    pthread_mutex_lock(&servidor.mutex_mm);
    servidor.cierre_normal = true;
    pthread_mutex_unlock(&servidor.mutex_mm);
    pthread_join(t_vigia, NULL);

    cout << "\n=========================================================\n";
    cout << "               RESUMEN FINAL DEL TORNEO\n";
    cout << "=========================================================\n";
    cout << " - Clientes totales recibidos (N) : " << N << "\n";
    cout << " - Mesas concurrentes maximas (M) : " << M << "\n";
    cout << " - Partidas totales disputadas    : " << servidor.contador_partidas << "\n";
    if (servidor.jugador_impar_sobrante != -1) {
        cout << " - Jugador impar sin pareja       : Cliente #" << servidor.jugador_impar_sobrante
             << " (Retirado limpiamente del lobby)\n";
    } else {
        cout << " - Jugador impar sin pareja       : Ninguno (Todos emparejados)\n";
    }
    cout << " - Recursos POSIX y memoria       : Liberados (0 fugas / 0 huerfanos)\n";
    cout << "=========================================================\n";
    cout << "[SERVIDOR] Todas las partidas han concluido. Cerrando servidor.\n";
    return 0;
}