#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"

static int fail_count = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fail_count++; } \
    else { printf("PASS: %s\n", msg); } \
} while(0)

static int callback_count;
static char last_result[1024];

static int callback(void *data, int argc, char **argv, char **cols) {
    (void)data; (void)cols;
    callback_count++;
    if (argc > 0 && argv[0]) {
        strncpy(last_result, argv[0], sizeof(last_result)-1);
        last_result[sizeof(last_result)-1] = 0;
    }
    return 0;
}

static int exec(sqlite3 *db, const char *sql) {
    char *err = NULL;
    callback_count = 0;
    last_result[0] = 0;
    int rc = sqlite3_exec(db, sql, callback, NULL, &err);
    if (rc != SQLITE_OK) {
        printf("  SQL error: %s\n", err);
        sqlite3_free(err);
    }
    return rc;
}

int main(void) {
    sqlite3 *db;
    int rc;

    printf("SQLite version: %s\n\n", sqlite3_libversion());

    rc = sqlite3_open(":memory:", &db);
    CHECK(rc == SQLITE_OK, "open in-memory database");

    // CREATE TABLE
    rc = exec(db, "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)");
    CHECK(rc == SQLITE_OK, "CREATE TABLE");

    // INSERT
    rc = exec(db, "INSERT INTO users VALUES (1, 'Alice', 30)");
    CHECK(rc == SQLITE_OK, "INSERT row 1");
    rc = exec(db, "INSERT INTO users VALUES (2, 'Bob', 25)");
    CHECK(rc == SQLITE_OK, "INSERT row 2");
    rc = exec(db, "INSERT INTO users VALUES (3, 'Charlie', 35)");
    CHECK(rc == SQLITE_OK, "INSERT row 3");

    // SELECT COUNT
    rc = exec(db, "SELECT COUNT(*) FROM users");
    CHECK(rc == SQLITE_OK && !strcmp(last_result, "3"), "SELECT COUNT(*) = 3");

    // SELECT with WHERE
    rc = exec(db, "SELECT name FROM users WHERE age > 28");
    CHECK(rc == SQLITE_OK && callback_count == 2, "SELECT WHERE age>28 returns 2 rows");

    // UPDATE
    rc = exec(db, "UPDATE users SET age = 31 WHERE name = 'Alice'");
    CHECK(rc == SQLITE_OK, "UPDATE");
    rc = exec(db, "SELECT age FROM users WHERE name = 'Alice'");
    CHECK(rc == SQLITE_OK && !strcmp(last_result, "31"), "verify UPDATE: age=31");

    // DELETE
    rc = exec(db, "DELETE FROM users WHERE name = 'Bob'");
    CHECK(rc == SQLITE_OK, "DELETE");
    rc = exec(db, "SELECT COUNT(*) FROM users");
    CHECK(rc == SQLITE_OK && !strcmp(last_result, "2"), "verify DELETE: count=2");

    // JOIN
    rc = exec(db, "CREATE TABLE orders (id INTEGER PRIMARY KEY, user_id INTEGER, item TEXT)");
    CHECK(rc == SQLITE_OK, "CREATE TABLE orders");
    rc = exec(db, "INSERT INTO orders VALUES (1, 1, 'Book')");
    exec(db, "INSERT INTO orders VALUES (2, 3, 'Pen')");
    rc = exec(db, "SELECT users.name FROM users JOIN orders ON users.id = orders.user_id");
    CHECK(rc == SQLITE_OK && callback_count == 2, "JOIN returns 2 rows");

    // INDEX
    rc = exec(db, "CREATE INDEX idx_age ON users(age)");
    CHECK(rc == SQLITE_OK, "CREATE INDEX");
    rc = exec(db, "SELECT name FROM users WHERE age = 31");
    CHECK(rc == SQLITE_OK && !strcmp(last_result, "Alice"), "indexed SELECT");

    // TRANSACTION
    rc = exec(db, "BEGIN TRANSACTION");
    CHECK(rc == SQLITE_OK, "BEGIN TRANSACTION");
    exec(db, "INSERT INTO users VALUES (4, 'Dave', 40)");
    rc = exec(db, "ROLLBACK");
    CHECK(rc == SQLITE_OK, "ROLLBACK");
    rc = exec(db, "SELECT COUNT(*) FROM users");
    CHECK(rc == SQLITE_OK && !strcmp(last_result, "2"), "verify ROLLBACK: count=2");

    // Subquery
    rc = exec(db, "SELECT name FROM users WHERE age = (SELECT MAX(age) FROM users)");
    CHECK(rc == SQLITE_OK && !strcmp(last_result, "Charlie"), "subquery: MAX(age)");

    // GROUP BY / HAVING
    exec(db, "INSERT INTO users VALUES (5, 'Eve', 35)");
    rc = exec(db, "SELECT age, COUNT(*) FROM users GROUP BY age HAVING COUNT(*) > 1");
    CHECK(rc == SQLITE_OK && callback_count == 1, "GROUP BY/HAVING");

    sqlite3_close(db);

    printf("\n%d test(s) failed.\n", fail_count);
    return fail_count ? 1 : 0;
}
