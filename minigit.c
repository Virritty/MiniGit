#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <dirent.h>
#include <windows.h>

#define HASH_BYTES          20
#define HASH_HEX_LEN        40
#define MAX_PATH_LEN        256
#define MAX_MESSAGE_LEN     256
#define MAX_BRANCH_NAME_LEN  64
#define OBJECTS_DIR         ".minigit/objects/"
#define REFS_DIR            ".minigit/refs/heads/"
#define HEAD_FILE           ".minigit/HEAD"

//СТРУКТУРЫ
//Узел дерева(файл или директория)
typedef struct TreeNode {
    char *path;                 //полный путь к файлу от корня репозитория
    char *blob_hash;            //хеш содержимого файла 
    struct TreeNode *next;      //указатель на следующий файл
} TreeNode;

//Коммит – неизменяемая версия
typedef struct Commit {
    char hash[HASH_HEX_LEN + 1];        //идентификатор коммита(вычисляется из содержимого всех файлов+сообщения+времени+родительского хеша) 
    char parent_hash[HASH_HEX_LEN + 1]; //хеш родительского коммита
    char message[MAX_MESSAGE_LEN];      //сообщение коммита 
    time_t timestamp;                   //время создания 
    TreeNode *files;                    //указатель на первый файл
    struct Commit *next;                //для связывания в историю 
} Commit;

//Простая таблица веток(имя -> хеш коммита)
typedef struct BranchEntry {
    char name[MAX_BRANCH_NAME_LEN];     //имя ветки
    char commit_hash[HASH_HEX_LEN + 1]; //хэш коммита
    struct BranchEntry *next;           //следующая ветка
} BranchEntry;

//Глобальные указатели
Commit *g_current_commit = NULL;
BranchEntry *g_branches = NULL;

//ФУНКЦИИ
//Хеширование содержимого(улучшенный DJB2)
static void content_hash(const unsigned char *data, size_t len, char out_hex[HASH_HEX_LEN + 1]) {
    unsigned long hash = 5381;
    for (size_t i = 0; i < len; i++) {
        hash = ((hash << 5) + hash) + data[i];
    }
    //Дополнительное перемешивание, чтобы получить 160 бит
    unsigned char bytes[HASH_BYTES];
    for (int i = 0; i < HASH_BYTES; i++) {
        bytes[i] = (hash >> (i * 8)) & 0xFF;
        hash = ((hash >> 1) ^ (hash << 7)) ^ 0x9e3779b9; 
    }
    for (int i = 0; i < HASH_BYTES; i++) {
        sprintf(out_hex + i * 2, "%02x", bytes[i]);
    }
    out_hex[HASH_HEX_LEN] = '\0';
}

//Хеширование коммита (на основе дерева файлов, родителя, сообщения, времени)
static void commit_hash(const Commit *c, char out_hex[HASH_HEX_LEN + 1]) {
    //Собираем все данные в один буфер 
    char buffer[4096];
    size_t pos = 0;
    for (TreeNode *n = c->files; n; n = n->next) {
        pos += snprintf(buffer + pos, sizeof(buffer) - pos, "%s|%s|", n->path, n->blob_hash);
        if (pos >= sizeof(buffer) - 1) break;
    }
    pos += snprintf(buffer + pos, sizeof(buffer) - pos, "|%s|%ld|%s", c->parent_hash, (long)c->timestamp, c->message);
    content_hash((unsigned char*)buffer, pos, out_hex);
}

//Получение полного пути к объекту (блобу) на диске по хешу
static void get_blob_path(const char *hash, char *out_path, size_t out_size) {
    snprintf(out_path, out_size, "%s%s", OBJECTS_DIR, hash);
}

//Сохранение блоба (данных) в файл по хешу, если ещё не существует
static bool blob_save(const char *data, size_t len, const char *hash) {
    char path[MAX_PATH_LEN];
    get_blob_path(hash, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return true; //Уже существует
    }
    f = fopen(path, "wb");
    if (!f) return false;
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    return written == len;
}

//Загрузка блоба (данные из файла) по хешу (выделяет память, вызывающий должен освободить)
static char* blob_load(const char *hash, size_t *out_len) {
    char path[MAX_PATH_LEN];
    get_blob_path(hash, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = (char*)malloc(len + 1);
    if (!data) { fclose(f); return NULL; }
    fread(data, 1, len, f); 
    fclose(f);
    data[len] = '\0';
    if (out_len) *out_len = len;
    return data;
}

//Сохранение коммита в файл .minigit/objects/<hash>.commit 
static bool commit_save(const Commit *c) {
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s%s.commit", OBJECTS_DIR, c->hash);
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "Родитель: %s\n", c->parent_hash);
    fprintf(f, "Сообщение: %s\n", c->message);
    fprintf(f, "Время: %ld\n", (long)c->timestamp);
    for (TreeNode *n = c->files; n; n = n->next) {
        fprintf(f, "Файл: %s %s\n", n->path, n->blob_hash);
    }
    fclose(f);
    return true;
}

//Загрузка коммита из файла и воссоздание структуры (выделяет память)
static Commit* commit_load(const char *hash) {
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s%s.commit", OBJECTS_DIR, hash);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    Commit *c = (Commit*)calloc(1, sizeof(Commit));
    if (!c) { fclose(f); return NULL; }
    strcpy(c->hash, hash); 
    c->timestamp = time(NULL);
    char line[1024];
    TreeNode **tail = &c->files;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Родитель: ", 10) == 0) {
            sscanf(line + 10, "%40s", c->parent_hash);
        } else if (strncmp(line, "Сообщение: ", 11) == 0) {
            char *nl = strchr(line + 11, '\n');
            int len = (nl ? nl - (line + 11) : (int)strlen(line + 11));
            if (len >= MAX_MESSAGE_LEN) len = MAX_MESSAGE_LEN - 1;
            strncpy(c->message, line + 11, len);
            c->message[len] = '\0';
        } else if (strncmp(line, "Время: ", 7) == 0) {
            sscanf(line + 7, "%ld", (long*)&c->timestamp);
        } else if (strncmp(line, "Файл: ", 6) == 0) {
            char path_buf[MAX_PATH_LEN], hash_buf[HASH_HEX_LEN + 1];
            if (sscanf(line + 6, "%s %s", path_buf, hash_buf) == 2) {
                TreeNode *node = (TreeNode*)malloc(sizeof(TreeNode));
                node->path = _strdup(path_buf);
                node->blob_hash = _strdup(hash_buf);
                node->next = NULL;
                *tail = node;
                tail = &node->next;
            }
        }
    }
    fclose(f);
    return c;
}

//Создание пустого дерево файлов
static TreeNode* tree_new(void) {
    return NULL;
}

//Создание новой версии списка, в котором один файл изменен/удалён/добавлен
static TreeNode* tree_set(TreeNode *old, const char *path, const char *blob_hash, bool *changed) {
    //Линейная реализация – ищем узел, если нашли и содержимое не совпадает – заменяем
    TreeNode *new_root = NULL;
    TreeNode **new_tail = &new_root;
    TreeNode *old_iter = old;
    bool found = false;
    while (old_iter) {
        if (strcmp(old_iter->path, path) == 0) {
            found = true;
            if (blob_hash == NULL) {
                //Удаление – пропускаем этот узел, сообщаем, что дерево изменилось
                *changed = true;
            } else if (strcmp(old_iter->blob_hash, blob_hash) != 0) {
                //Замена
                TreeNode *node = (TreeNode*)malloc(sizeof(TreeNode)); 
                node->path = _strdup(path);
                node->blob_hash = _strdup(blob_hash);
                node->next = NULL;
                *new_tail = node;
                new_tail = &node->next;
                *changed = true;
            } else {
                //Без изменений, значит копируем старый узел в новом узле
                TreeNode *node = (TreeNode*)malloc(sizeof(TreeNode));
                node->path = _strdup(old_iter->path);
                node->blob_hash = _strdup(old_iter->blob_hash);
                node->next = NULL;
                *new_tail = node;
                new_tail = &node->next;
            }
        } else {
            //Копируем старый узел
            TreeNode *node = (TreeNode*)malloc(sizeof(TreeNode));
            node->path = _strdup(old_iter->path);
            node->blob_hash = _strdup(old_iter->blob_hash);
            node->next = NULL;
            *new_tail = node;
            new_tail = &node->next;
        }
        old_iter = old_iter->next;
    }
    if (!found && blob_hash != NULL) {
        //Добавление нового файла
        TreeNode *node = (TreeNode*)malloc(sizeof(TreeNode));
        node->path = _strdup(path);
        node->blob_hash = _strdup(blob_hash);
        node->next = NULL;
        *new_tail = node;
        *changed = true;
    } else if (!found && blob_hash == NULL) {
        *changed = false;
    }
    //Старое дерево может использоваться в других коммитах, мы его не удаляем
    return new_root;
}

//Получение хеша блоба (данных) из дерева по пути
static char* tree_get_blob(TreeNode *tree, const char *path) {
    for (TreeNode *n = tree; n; n = n->next) {
        if (strcmp(n->path, path) == 0)
            return n->blob_hash;
    }
    return NULL;
}

//Удаление всего дерева
static void tree_free(TreeNode *tree) {
    while (tree) {
        TreeNode *next = tree->next;
        free(tree->path);
        free(tree->blob_hash);
        free(tree);
        tree = next;
    }
}

//ОСНОВНЫЕ API
//Cоздание пустого репозитория и начального коммита
Commit* init_repo(void) {
    _mkdir(".minigit");
    _mkdir(".minigit/objects");
    _mkdir(".minigit/refs");
    _mkdir(".minigit/refs/heads");
    Commit *c = (Commit*)calloc(1, sizeof(Commit));
    strcpy(c->parent_hash, "");
    strcpy(c->message, "Первый коммит");
    c->timestamp = time(NULL);
    c->files = tree_new();
    commit_hash(c, c->hash);
    commit_save(c);
    return c;
}

//Возвращение нового коммита с добавленным/изменённым файлом
Commit* add_file(Commit *old_commit, const char *path, const char *content) {
    if (!old_commit) return NULL;
    char hash[HASH_HEX_LEN + 1];
    content_hash((unsigned char*)content, strlen(content), hash);
    blob_save(content, strlen(content), hash);
    bool changed = false;
    TreeNode *new_tree = tree_set(old_commit->files, path, hash, &changed);
    if (!changed) {
        tree_free(new_tree);
        return (Commit*)old_commit; 
    }
    Commit *c = (Commit*)calloc(1, sizeof(Commit));
    strcpy(c->parent_hash, old_commit->hash);
    strcpy(c->message, "");
    c->timestamp = time(NULL);
    c->files = new_tree;
    commit_hash(c, c->hash);
    commit_save(c);
    return c;
}

//Удаление файла из новой версии 
Commit* remove_file(Commit *old_commit, const char *path) {
    if (!old_commit) return NULL;
    bool changed = false;
    TreeNode *new_tree = tree_set(old_commit->files, path, NULL, &changed);
    if (!changed) return (Commit*)old_commit;
    Commit *c = (Commit*)calloc(1, sizeof(Commit));
    strcpy(c->parent_hash, old_commit->hash);
    strcpy(c->message, "");
    c->timestamp = time(NULL);
    c->files = new_tree;
    commit_hash(c, c->hash);
    commit_save(c);
    return c;
}

//Фиксирование состояния с сообщением 
Commit* commit(Commit *state, const char *message) {
    if (!state) return NULL;
    Commit *c = (Commit*)malloc(sizeof(Commit));
    memcpy(c, state, sizeof(Commit));
    strncpy(c->message, message, MAX_MESSAGE_LEN - 1);
    c->message[MAX_MESSAGE_LEN - 1] = '\0';
    c->timestamp = time(NULL);
    commit_hash(c, c->hash);
    commit_save(c);
    return c;
}

//Возвращение содержимого файла из указанной версии
char* get_file_content(Commit *commit, const char *path, size_t *out_len) {
    if (!commit) return NULL;
    char *blob_hash = tree_get_blob(commit->files, path);
    if (!blob_hash) return NULL;
    return blob_load(blob_hash, out_len);
}

//Проверка на существование файла
bool get_file_exists(Commit *commit, const char *path) {
    if (!commit) return false;
    return tree_get_blob(commit->files, path) != NULL;
}

//Вывод на экран информации о коммите
void print_commit(Commit *commit) {
    if (!commit) { printf("Коммит не существует\n"); return; }
    printf("Хеш: %s\n", commit->hash);
    printf("Родитель: %s\n", commit->parent_hash);
    printf("Сообщение: %s\n", commit->message);
    printf("Дата: %s", ctime(&commit->timestamp));
    printf("Файлы:\n");
    for (TreeNode *n = commit->files; n; n = n->next) {
        printf("  %s [%s]\n", n->path, n->blob_hash);
    }
}

//Вывод истории коммитов с указанного до первого
void print_history(Commit *commit) {
    while (commit) {
        printf("\n=== Коммит %s ===\n", commit->hash);
        printf("Сообщение: %s\n", commit->message);
        printf("Дата: %s", ctime(&commit->timestamp));
        if (strlen(commit->parent_hash) == 0) break;
        //Загружаем родителя из хранилища 
        Commit *parent = commit_load(commit->parent_hash);
        if (!parent) break;
        commit = parent;
    }
}

//Вывод списка всех файлов
void print_files(Commit *commit) {
    if (!commit) return;
    printf("Файлы в коммите %s:\n", commit->hash);
    for (TreeNode *n = commit->files; n; n = n->next) {
        printf("  %s\n", n->path);
    }
}

//Создание новой ветки
bool create_branch(const char *branch_name, Commit *commit) {
    if (!commit) return false;
    BranchEntry *entry = (BranchEntry*)malloc(sizeof(BranchEntry));
    strncpy(entry->name, branch_name, MAX_BRANCH_NAME_LEN - 1);
    entry->name[MAX_BRANCH_NAME_LEN - 1] = '\0';
    strcpy(entry->commit_hash, commit->hash);
    entry->next = g_branches;
    g_branches = entry;
    //Сохраняем в файл для персистентности
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s%s", REFS_DIR, branch_name);
    FILE *f = fopen(path, "w");
    if (f) { 
        fprintf(f, "%s", commit->hash); 
        fclose(f); 
    }
    return true;
}

//Указатель на коммит, на который указывает ветка
Commit* get_branch_head(const char *branch_name) {
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s%s", REFS_DIR, branch_name);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char hash[HASH_HEX_LEN + 1];
    if (fscanf(f, "%40s", hash) != 1) { fclose(f); return NULL; }
    fclose(f);
    return commit_load(hash);
}

//Установка нового текущего коммита и его сохранение 
bool checkout(Commit *commit) {
    if (!commit) return false;
    g_current_commit = commit;
    //Сохраняем HEAD
    FILE *f = fopen(HEAD_FILE, "w");
    if (f) { 
        fprintf(f, "%s", commit->hash); 
        fclose(f); 
    }
    return true;
}

//Подсчёт уникальных блобов и коммитов 
static void count_objects_rec(Commit *c, bool *commits_set, bool *blobs_set, int *commits_cnt, int *blobs_cnt) {
    if (!c || commits_set[0]) return;
    if (!commits_set[0]) {
        commits_set[0] = true;
        (*commits_cnt)++;
    }
    for (TreeNode *n = c->files; n; n = n->next) {
        //Каждый хеш файла – уникальный блоб 
        bool found = false;
        //Поиск в глобальном массиве
        static char seen[1000][HASH_HEX_LEN + 1];
        static int seen_cnt = 0;
        for (int i = 0; i < seen_cnt; i++) {
            if (strcmp(seen[i], n->blob_hash) == 0) { 
                found = true; 
                break; 
            }
        }
        if (!found && seen_cnt < 1000) {
            strcpy(seen[seen_cnt++], n->blob_hash);
            (*blobs_cnt)++;
        }
    }
    if (strlen(c->parent_hash)) {
        Commit *parent = commit_load(c->parent_hash);
        if (parent) count_objects_rec(parent, commits_set, blobs_set, commits_cnt, blobs_cnt);
    }
}

//Запуск подсчёта объектов в репозитории
int count_objects(Commit *start) {
    bool commits_set = false;
    bool blobs_set = false;
    int commits_cnt = 0, blobs_cnt = 0;
    count_objects_rec(start, &commits_set, &blobs_set, &commits_cnt, &blobs_cnt);
    return commits_cnt + blobs_cnt;
}

//Экономия памяти при создании нового коммита
void show_memory_saving(Commit *old_c, Commit *new_c) {
    if (!old_c || !new_c) return;
    int old_files = 0, shared = 0;
    for (TreeNode *n = old_c->files; n; n = n->next) old_files++;
    for (TreeNode *n = new_c->files; n; n = n->next) {
        for (TreeNode *m = old_c->files; m; m = m->next) {
            if (strcmp(n->path, m->path) == 0 && strcmp(n->blob_hash, m->blob_hash) == 0) {
                shared++;
                break;
            }
        }
    }
    printf("Экономия памяти: %d общих блобов из %d имеющихся (%.1f%% повторно использованных)\n",
           shared, old_files + 1, (shared * 100.0) / (old_files + 1));
}

//CLI
static void instructions(void) {
    printf("Как использовать: minigit.exe <command> [args...]\n");
    printf("Доступные команды:\n");
    printf("init_repo                         |Создание репозитория\n");
    printf("add_file <path> <content>         |Добавление/изменение файла\n");
    printf("remove_file <path>                |Удаление файла\n");
    printf("commit <message>                  |Добавление сообщения к коммиту\n");
    printf("get_file_content <path>           |Показать содержимое файла\n");
    printf("get_file_exists <path>            |Проверка существует ли файл\n");
    printf("print_commit <hash>               |Вся информация о коммите\n");
    printf("print_history [hash]              |Просмотр истории\n");
    printf("print_files                       |Список всех файлов в текущей версии\n");
    printf("create_branch <name> <commit_hash>|Создание новой ветки на коммит\n");
    printf("get_branch_head <name>            |Хеш коммита, на который указывает ветка\n");
    printf("checkout <commit_hash>            |Переключение на коммит\n");
    printf("count_objects                     |Подсчёт уникальных элементов\n");
    printf("show_memory_saving                |Показать экономию памяти\n");
}

int main(int argc, char *argv[]) {
    SetConsoleOutputCP(CP_UTF8);     
    SetConsoleCP(CP_UTF8);

    if (argc < 2) {
        instructions();
        return 1;
    }
    //Загружаем последний HEAD, если есть
    FILE *head_f = fopen(HEAD_FILE, "r");
    if (head_f) {
        char hash[HASH_HEX_LEN + 1];
        if (fscanf(head_f, "%40s", hash) == 1) {
            g_current_commit = commit_load(hash);
        }
        fclose(head_f);
    }
    if (!g_current_commit && strcmp(argv[1], "init_repo") != 0) {
        printf("Репозиторий не найден. Используйте 'init_repo'.\n");
        return 1;
    }

    if (strcmp(argv[1], "init_repo") == 0) {
        g_current_commit = init_repo();
        checkout(g_current_commit);
        printf("Репозиторий создан.\n");
    }
    else if (strcmp(argv[1], "add_file") == 0 && argc >= 4) {
        const char *path = argv[2];
        const char *content = argv[3];
        g_current_commit = add_file(g_current_commit, path, content);
        checkout(g_current_commit);
        printf("Файл '%s' добавлен/обновлен.\n", path);
    }
    else if (strcmp(argv[1], "remove_file") == 0 && argc >= 3) {
        const char *path = argv[2];
        g_current_commit = remove_file(g_current_commit, path);
        checkout(g_current_commit);
        printf("Файл '%s' удалён.\n", path);
    }
    else if (strcmp(argv[1], "commit") == 0 && argc >= 3) {
        const char *msg = argv[2];
        Commit *c = commit(g_current_commit, msg);
        checkout(c);
        printf("Сохранён: %s\n", c->hash);
    }
    else if (strcmp(argv[1], "get_file_content") == 0 && argc >= 3) {
        size_t len;
        char *content = get_file_content(g_current_commit, argv[2], &len);
        if (content) { printf("%s\n", content); free(content); }
        else printf("Файл не найден.\n");
    }
    else if (strcmp(argv[1], "get_file_exists") == 0 && argc >= 3) {
        bool exists = get_file_exists(g_current_commit, argv[2]);
        printf("%s\n", exists ? "True" : "False");
    }
    else if (strcmp(argv[1], "print_commit") == 0 && argc >= 3) {
        Commit *c = commit_load(argv[2]);
        if (c) { print_commit(c); free(c); }
        else printf("Коммит не найден.\n");
    }
    else if (strcmp(argv[1], "print_history") == 0) {
        Commit *start = g_current_commit;
        if (argc >= 3) start = commit_load(argv[2]);
        if (start) print_history(start);
        else printf("Коммит не найден.\n");
    }
    else if (strcmp(argv[1], "print_files") == 0) {
        print_files(g_current_commit);
    }
    else if (strcmp(argv[1], "create_branch") == 0 && argc >= 4) {
        Commit *c = commit_load(argv[3]);
        if (c && create_branch(argv[2], c)) printf("Бранч '%s' создан.\n", argv[2]);
        else printf("Ошибка.\n");
    }
    else if (strcmp(argv[1], "get_branch_head") == 0 && argc >= 3) {
        Commit *c = get_branch_head(argv[2]);
        if (c) printf("Ветка '%s' ведёт к %s\n", argv[2], c->hash);
        else printf("Ветка не найдена.\n");
    }
    else if (strcmp(argv[1], "checkout") == 0 && argc >= 3) {
        Commit *c = commit_load(argv[2]);
        if (c && checkout(c)) printf("Переход к коммиту %s\n", c->hash);
        else printf("Проверка не выполнена.\n");
    }
    else if (strcmp(argv[1], "count_objects") == 0) {
        int total = count_objects(g_current_commit);
        printf("Всего уникальных объектов: %d\n", total);
    }
    else if (strcmp(argv[1], "show_memory_saving") == 0) {
        Commit *parent = NULL;
        if (strlen(g_current_commit->parent_hash))
            parent = commit_load(g_current_commit->parent_hash);
        if (parent) show_memory_saving(parent, g_current_commit);
        else printf("Нет родительских коммитов для сравнения.\n");
    }
    else {
        instructions();
    }
    return 0;
}