// #include <cstdint>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define COLUMN_USER_SIZE 32
#define COLUMN_EMAIL_SIZE 255

typedef struct
{
    __uint32_t id;
    char username[COLUMN_USER_SIZE + 1];
    char email[COLUMN_EMAIL_SIZE + 1];
} Row;

typedef struct
{
    char* buffer;
    size_t buffer_length;
    ssize_t input_length;
} InputBuffer;

typedef enum
{
    META_COMMAND_SUCCESS,
    META_COMMAND_UNRECOGNIZED_COMMAND
} MetaCommandResult;

typedef enum
{
    PREPARE_SUCCESS,
    PREPARE_STRING_TOO_LONG,
    PREPARE_NEGATIVE_ID,
    PREPARE_UNRECOGNIZED_STATEMENT,
    PREPARE_SYNTAX_ERROR
} PrepareResult;

typedef enum
{
    STATEMENT_INSERT,
    STATEMENT_SELECT
} StatementType;

typedef enum
{
    EXECUTE_SUCCESS,
    EXECUTE_TABLE_FULL
} ExecuteResult;

typedef struct
{
    StatementType type;
    Row row_to_insert;
} Statement;

#define size_of_attribute(Struct, Attribute) sizeof(((Struct*)0)->Attribute)

const __uint32_t ID_SIZE = size_of_attribute(Row, id);
const __uint32_t USERNAME_SIZE = size_of_attribute(Row, username);
const __uint32_t EMAIL_SIZE = size_of_attribute(Row, email);
const __uint32_t ID_OFFSET = 0;
const __uint32_t USERNAME_OFFSET = ID_OFFSET + ID_SIZE;
const __uint32_t EMAIL_OFFSET = USERNAME_OFFSET + USERNAME_SIZE;
const __uint32_t ROW_SIZE = ID_SIZE + USERNAME_SIZE + EMAIL_SIZE;

InputBuffer* new_input_buffer()
{
    InputBuffer* input_buffer = (InputBuffer*)malloc(sizeof(InputBuffer));
    input_buffer->buffer = NULL;
    input_buffer->buffer_length=0;
    input_buffer->input_length=0;

    return input_buffer;
}

void serialize_row(Row* source, void* destination)
{
    memcpy(destination + ID_OFFSET, &(source->id), ID_SIZE);
    memcpy(destination + USERNAME_OFFSET, &(source->username), USERNAME_SIZE);
    memcpy(destination + EMAIL_OFFSET, &(source->email), EMAIL_SIZE);
}

void deserialize_row(void* source, Row* destination)
{
    memcpy(&(destination->id), source + ID_OFFSET, ID_SIZE);
    memcpy(&(destination->username), source + USERNAME_OFFSET, USERNAME_SIZE);
    memcpy(&(destination->email), source + EMAIL_OFFSET, EMAIL_SIZE);
}

const __uint32_t PAGE_SIZE = 4096;
#define TABLE_MAX_PAGES 100
const __uint32_t ROWS_PER_PAGE = PAGE_SIZE / ROW_SIZE;
const __uint32_t TABLE_MAX_ROWS = ROWS_PER_PAGE * TABLE_MAX_PAGES;

typedef struct
{
    int file_descriptor;
    __uint32_t file_length;
    void* pages[TABLE_MAX_PAGES];
} Pager;

typedef struct
{
    __uint32_t num_rows;
    Pager* pager;
} Table;

void* get_page(Pager* pager, __uint32_t page_num)
{
    if (page_num >= TABLE_MAX_PAGES)
    {
        printf("Tried to fetch page number out of bounds");
        exit(EXIT_FAILURE);
    }

    if (pager->pages[page_num] == NULL)
    {
     // Not in cache therefore fetch from file
        void* page = malloc(PAGE_SIZE);
        __uint32_t num_page = pager->file_length/PAGE_SIZE;

        if (pager->file_length%PAGE_SIZE)
        {
            num_page++;
        }

        if (page_num <= num_page)
        {
            lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET);
            ssize_t bytes_read = read(pager->file_descriptor, page, PAGE_SIZE);
            if (bytes_read==-1)
            {
                printf("Error reading file");
                exit(EXIT_FAILURE);
            }
        }
        pager->pages[page_num] = page;
    }
    return pager->pages[page_num];
}

void pager_flush(Pager* pager, int page_num, int size) {

    if (pager->pages[page_num] == NULL) {
            printf("Tried to flush null page\n");
            exit(EXIT_FAILURE);
    }

    off_t offset = lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET);

    if (offset == -1) {
        printf("Error seeking: %d\n", errno);
        exit(EXIT_FAILURE);
    }

    ssize_t bytes_written =write(pager->file_descriptor, pager->pages[page_num], size);
    if (bytes_written == -1) {
            printf("Error writing: %d\n", errno);
            exit(EXIT_FAILURE);
    }
}


void db_close(Table* table)
{
    Pager* pager = table->pager;
    __uint32_t num_full_pages = table->num_rows/ROWS_PER_PAGE;

    for (int i = 0; i < num_full_pages; i++)
    {
        if (pager->pages[i]==NULL)
        {
            continue;
        }

        pager_flush(pager,i,PAGE_SIZE);
        free(pager->pages[i]);
        pager->pages[i] = NULL;

        int num_additional_rows = table->num_rows%ROWS_PER_PAGE;
        if ( num_additional_rows>0)
        {
            int page_num = num_full_pages;
            if (pager->pages[page_num]==NULL)
            {
                pager_flush(pager,page_num,num_additional_rows*ROW_SIZE);
                free(pager->pages[page_num]);
                pager->pages[page_num] = NULL;
            }
        }

        int result = close(pager->file_descriptor);
        if (result==-1)
        {
            printf("Error closing file");
            exit(EXIT_FAILURE);
        }

        for (int i = 0; i < TABLE_MAX_ROWS; i++)
        {
            void* page = pager->pages[i];
            if (page)
            {
                free(page);
                pager->pages[i] = NULL;
            }
        }

        free(pager);
        free(table);

    }
}

void* row_slot(Table* table, __uint32_t row_num)
{
    __uint32_t page_num = row_num / ROWS_PER_PAGE;
    void* page = get_page(table->pager,page_num);
    __uint32_t row_offset = page_num * ROWS_PER_PAGE;
    __uint32_t byte_offset = row_offset * ROW_SIZE;
    return page + byte_offset;
}

void print_prompt() {printf("db >");}

void print_row(Row* row) {printf("%d , %s , %s \n",row->id,row->username,row->email);}

void read_input(InputBuffer* input_buffer)
{
    ssize_t bytes_read = getline(&(input_buffer->buffer), &(input_buffer->buffer_length),stdin); // allocate memory to store the input

    if (bytes_read<=0)
    {
        printf("Error reading input");
        exit(EXIT_FAILURE);
    }

    input_buffer->input_length=bytes_read-1;
    input_buffer->buffer[bytes_read-1]=0;

}

void close_input_buffer(InputBuffer* input_buffer)
{
    free(input_buffer->buffer);
    free(input_buffer);
}

// handle meta commands (commands that start with a .)
MetaCommandResult do_meta_command(InputBuffer* input_buffer, Table* table)
{
    if (strcmp(input_buffer->buffer, ".exit")==0)
    {
        db_close(table);
        exit(EXIT_SUCCESS);
    } else
    {
        return META_COMMAND_UNRECOGNIZED_COMMAND;
    }
}

PrepareResult prepare_insert(InputBuffer* input_buffer, Statement* statement)
{
    statement->type = STATEMENT_INSERT;

    char* keyword  = strtok(input_buffer->buffer, " ");
    char* id_string = strtok(NULL, " ");
    char* username  = strtok(NULL, " ");
    char* email    = strtok(NULL, " ");

    if (id_string==NULL || username==NULL || email==NULL)
    {
        return PREPARE_SYNTAX_ERROR;
    }

    int id = atoi(id_string); // atoi --> ASCII to Int
    if (id < 0)
    {
        return PREPARE_NEGATIVE_ID;
    }
    if (strlen(username) > COLUMN_USER_SIZE)
    {
        return PREPARE_STRING_TOO_LONG;
    }
    if (strlen(email) > COLUMN_EMAIL_SIZE)
    {
        return PREPARE_STRING_TOO_LONG;
    }

    statement->row_to_insert.id = id;
    strcpy(statement->row_to_insert.username, username);
    strcpy(statement->row_to_insert.email, email);

    return PREPARE_SUCCESS;

}

PrepareResult prepare_statement(InputBuffer* input_buffer, Statement* statement)
{
    if (strncmp(input_buffer->buffer, "insert", 6)==0)
    {
        return prepare_insert(input_buffer, statement);
    }
    if (strcmp(input_buffer->buffer, "select")==0)
    {
        statement->type = STATEMENT_SELECT;
        return PREPARE_SUCCESS;
    }
    return PREPARE_UNRECOGNIZED_STATEMENT;
}

ExecuteResult execute_insert(Statement* statement, Table* table)
{
    if (table->num_rows >= TABLE_MAX_ROWS)
    {
        return EXECUTE_TABLE_FULL;
    }

    Row* row_to_insert = &(statement->row_to_insert);

    serialize_row(row_to_insert, row_slot(table,table->num_rows));
    table->num_rows++;
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_select(Statement* statement, Table* table)
{
    Row row;
    for (int i=0; i<table->num_rows; i++)
    {
        deserialize_row(row_slot(table,i), &row);
        print_row(&row);
    }
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_statement(Statement* statement, Table* table)
{
    switch (statement->type)
    {
        case (STATEMENT_INSERT):
            return execute_insert(statement,table);
        case (STATEMENT_SELECT):
            return execute_select(statement,table);
    }
}

Pager* pager_open(const char* filename)
{
    int fd = open(filename,O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);

    if (fd == -1)
    {
        printf("Error opening file");
        exit(EXIT_FAILURE);
    }

    off_t file_size = lseek(fd,0,SEEK_END);

    Pager* pager = malloc(sizeof(Pager));
    pager->file_descriptor = fd;
    pager->file_length = file_size;

    for (int i=0; i < TABLE_MAX_PAGES; i++)
    {
        pager->pages[i] = NULL;
    }

    return pager;
}

Table* db_open(const char* filename)
{
    Pager* pager = pager_open(filename);
    __uint32_t num_rows = pager->file_length / ROW_SIZE;

    Table* table = (Table*)malloc(sizeof(Table));
    table->pager = pager;
    table->num_rows = num_rows;
    for (int i=0; i<TABLE_MAX_PAGES; i++)
    {
        pager->pages[i] = NULL;
    }
    return table;
}

int main(int argc, char *argv[])
{
    argc = scanf( "%d", &argc );
    if (argc < 2)
    {
        printf("Please give a filename\n");
        exit(EXIT_FAILURE);
    }

    char* filename = argv[1];
    Table* table = db_open(filename);

    InputBuffer* input_buffer = new_input_buffer();
    while (true)
    {
        print_prompt();
        read_input(input_buffer);

        if (input_buffer->buffer[0]=='.')
        {
            switch (do_meta_command(input_buffer, table))
            {
            case META_COMMAND_SUCCESS:
                continue;
            case META_COMMAND_UNRECOGNIZED_COMMAND:
                printf("Unrecognized command `%s'\n", input_buffer->buffer);
                continue;
            }
        }

        Statement statement;

        switch (prepare_statement(input_buffer, &statement))
        {
            case (PREPARE_SUCCESS):
                break;
            case (PREPARE_SYNTAX_ERROR):
                printf("Syntax error could not parse statement.");
                continue;
            case (PREPARE_STRING_TOO_LONG):
                printf("String too long!");
                continue;
            case (PREPARE_NEGATIVE_ID):
                printf("Cannot have a negative ID number");
                continue;
            case (PREPARE_UNRECOGNIZED_STATEMENT):
                printf("Unrecognized keyword `%s'\n", input_buffer->buffer);
                continue;
        }
        switch (execute_statement(&statement, table))
        {
            case (EXECUTE_SUCCESS):
                printf("Executed!");
                break;
            case (EXECUTE_TABLE_FULL):
                printf("Table full!");
            break;

        }
    }
}