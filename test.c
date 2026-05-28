#include <windows.h>
#define main _minigit_main
#include "minigit.c"

#undef main

//Сброс глобального состояния
static void reset_repo(void) {
    system("rmdir /s /q .minigit 2>nul");
    g_current_commit = NULL;
    g_branches = NULL;
}

//Вспомогательные функции тестирования
static int tests_passed = 0;
static int tests_failed = 0;

static void assert_true(const char *name, bool condition) {
    if (condition) {
        printf("Выполнено: %s\n", name);
        tests_passed++;
    } else {
        printf("Не выполнено: %s\n", name);
        tests_failed++;
    }
}

static void assert_false(const char *name, bool condition) {
    assert_true(name, !condition);
}

static void assert_string_equal(const char *name, const char *expected, const char *actual) {
    if ((expected == NULL && actual == NULL) ||
        (expected != NULL && actual != NULL && strcmp(expected, actual) == 0)) {
        printf("Выполнено: %s\n", name);
        tests_passed++;
    } else {
        printf("Не выполнено: %s (ожидаемый '%s', полученный '%s')\n", 
               name, expected ? expected : "NULL", actual ? actual : "NULL");
        tests_failed++;
    }
}

//Тест 1
static void test_init_repo(void) {
    printf("\nTest 1: init_repo\n");
    reset_repo();
    
    Commit *c = init_repo();
    assert_true("init_repo результат не NULL", c != NULL);
    assert_true("parent_hash пустой", strlen(c->parent_hash) == 0);
    assert_string_equal("Сообщение", "Первый коммит", c->message);
    assert_true("длина хеша 40", strlen(c->hash) == 40);
}

//Тест 2
static void test_add_and_get_file(void) {
    printf("\nTest 2: add_file и get_file_content\n");
    reset_repo();
    Commit *c1 = init_repo();
    Commit *c2 = add_file(c1, "test.txt", "Hello World");
    
    assert_true("add_file возвращает новый коммит", c2 != c1);
    
    size_t len;
    char *content = get_file_content(c2, "test.txt", &len);
    assert_string_equal("get_file_content возвращает корректное соединение", "Hello World", content);
    free(content);
    
    assert_true("get_file_exists возвращает true", get_file_exists(c2, "test.txt"));
    assert_false("get_file_exists для потерянных файлов возвращает false", get_file_exists(c2, "no.txt"));
}

//Тест 3
static void test_persistence(void) {
    printf("\nTest 3: персистентность\n");
    reset_repo();
    Commit *c1 = init_repo();
    Commit *c2 = add_file(c1, "test.txt", "Version 1");
    Commit *c3 = add_file(c2, "test.txt", "Version 2");
    
    size_t len;
    char *c2_content = get_file_content(c2, "test.txt", &len);
    char *c3_content = get_file_content(c3, "test.txt", &len);
    
    assert_string_equal("c2 содержит 'Version 1'", "Version 1", c2_content);
    assert_string_equal("c3 содержит 'Version 2'", "Version 2", c3_content);
    assert_true("разный хеш", strcmp(c2->hash, c3->hash) != 0);
    
    free(c2_content);
    free(c3_content);
}

//Тест 4
static void test_remove_file(void) {
    printf("\nTest 4: remove_file\n");
    reset_repo();
    Commit *c1 = init_repo();
    Commit *c2 = add_file(c1, "test.txt", "Content");
    Commit *c3 = remove_file(c2, "test.txt");
    
    assert_true("remove_file возвращает новый коммит", c3 != c2);
    assert_false("файл удалён из нового коммита", get_file_exists(c3, "test.txt"));
    assert_true("файл остается в старом коммите", get_file_exists(c2, "test.txt"));
}

//Тест 5
static void test_commit(void) {
    printf("\nTest 5: commit\n");
    reset_repo();
    Commit *c1 = init_repo();
    Commit *c2 = add_file(c1, "test.txt", "Content");
    Commit *c3 = commit(c2, "Test message");
    
    assert_true("commit возвращает новый коммит", c3 != c2);
    assert_string_equal("сообщение сохранено", "Test message", c3->message);
    assert_true("хеш изменен", strcmp(c2->hash, c3->hash) != 0);
    assert_true("файл сохранен", get_file_exists(c3, "test.txt"));
}

//Тест 6
static void test_branches(void) {
    printf("\nTest 6: create_branch и get_branch_head\n");
    reset_repo();
    Commit *c1 = init_repo();
    Commit *c2 = add_file(c1, "test.txt", "Content");
    
    bool created = create_branch("master", c2);
    assert_true("create_branch возвращает true", created);
    
    Commit *head = get_branch_head("master");
    assert_true("get_branch_head возвращает не NULL", head != NULL);
    assert_string_equal("ветки для правильного коммита", c2->hash, head->hash);
    
    free(head);
}

//Тест 7
static void test_checkout(void) {
    printf("\nTest 7: checkout\n");
    reset_repo();
    Commit *c1 = init_repo();
    Commit *c2 = add_file(c1, "test.txt", "Version 1");
    Commit *c3 = add_file(c2, "test.txt", "Version 2");
    
    bool ok = checkout(c2);
    assert_true("checkout возвращает true", ok);
    assert_true("g_current_commit изменен", g_current_commit == c2);
    
    size_t len;
    char *content = get_file_content(g_current_commit, "test.txt", &len);
    assert_string_equal("после checkout возвращается версия из c2", "Version 1", content);
    free(content);
}

//Тест 8
static void test_count_objects(void) {
    printf("\nTest 8: count_objects\n");
    reset_repo();
    Commit *c1 = init_repo();
    Commit *c2 = add_file(c1, "a.txt", "AAA");
    Commit *c3 = add_file(c2, "b.txt", "BBB");
    Commit *c4 = add_file(c3, "a.txt", "CCC");
    
    int total = count_objects(c4);
    assert_true("колличество объектов > 0", total > 0);
    printf("  count_objects возвращает: %d\n", total);
}

//Тест 9:
static void test_memory_saving(void) {
    printf("\nTest 9: show_memory_saving\n");
    reset_repo();
    Commit *c1 = init_repo();
    Commit *c2 = add_file(c1, "a.txt", "AAA");
    Commit *c3 = add_file(c2, "b.txt", "BBB");
    Commit *c4 = add_file(c3, "a.txt", "CCC");
    
    show_memory_saving(c3, c4);
    printf("(Проверьте вывод)\n");
    tests_passed++;
}

//Тест 10
static void test_print_files(void) {
    printf("\nTest 10: print_files\n");
    reset_repo();
    Commit *c1 = init_repo();
    Commit *c2 = add_file(c1, "a.txt", "AAA");
    Commit *c3 = add_file(c2, "b.txt", "BBB");
    
    print_files(c3);
    printf("(Проверьте вывод)\n");
    tests_passed++;
}

// Главная функция
int main(void) {
    SetConsoleOutputCP(CP_UTF8);     
    SetConsoleCP(CP_UTF8);

    printf("Тестирование MiniGit\n");
    
    test_init_repo();
    test_add_and_get_file();
    test_persistence();
    test_remove_file();
    test_commit();
    test_branches();
    test_checkout();
    test_count_objects();
    test_memory_saving();
    test_print_files();
    
    printf("Результаты: %d выполнено, %d не выполнено\n", tests_passed, tests_failed);
    
    reset_repo();
    
    return tests_failed == 0 ? 0 : 1;
}