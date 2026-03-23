/*
 * @Author: error: error: git config user.name & please set dead value or install git && error: git config user.email & please set dead value or install git & please set dead value or install git
 * @Date: 2023-10-10 10:03:37
 * @LastEditors: error: error: git config user.name & please set dead value or install git && error: git config user.email & please set dead value or install git & please set dead value or install git
 * @LastEditTime: 2024-01-18 12:01:00
 * @FilePath: /project_2/main.c
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>

typedef void (*printer_print_fn)(void *printer, const char *str);
struct printer_i
{
    printer_print_fn print;
};

struct plain_printer
{
    const struct printer_i *interface;
    const char *prefix;
};
void plain_printer_print(struct plain_printer *self, const char *str);
static const struct printer_i printer_interface = {
    .print = (printer_print_fn)plain_printer_print,
};
struct plain_printer *plain_printer_new(const char *prefix)
{
    struct plain_printer *self;
    self = malloc(sizeof(struct plain_printer));
    assert(self != NULL);

    self->interface = &printer_interface;
    self->prefix = prefix;
    return self;
}
void plain_printer_cleanup(struct plain_printer *self)
{
    free(self);
}
void plain_printer_print(struct plain_printer *self, const char *str)
{
    printf("%s%s", self->prefix, str);
}
#if 1
struct color_printer
{
    const struct printer_i *interface;
    int enable_color;
    const char *color_command;
    char *buf;
};
void color_printer_print(struct color_printer *self, const char *str);
static const struct printer_i printer_interface1 = {
    .print = (printer_print_fn)color_printer_print,
};
struct color_printer *color_printer_new(const char *color_command)
{
    struct color_printer *self;
    self = malloc(sizeof(struct color_printer));

    self->interface = &printer_interface1;
    self->color_command = color_command == NULL ? "\033[31:40m" : color_command;
    self->enable_color = 1;
    self->buf = malloc(100);

    return self;
}
void color_printer_cleanup(struct color_printer *self)
{
    free(self->buf);
    free(self);
}
void color_printer_print(struct color_printer *self, const char *str)
{
    if (self->enable_color)
        printf("%s%s\033[0m", self->color_command, str);
    else
        printf("%s", str);
}
void color_pprinter_disable_color(struct color_printer *self)
{
    self->enable_color = 0;
}
#endif
int main(int argc, char *argv[])
{
    struct plain_printer *p1;
    struct plain_printer *p2;
    struct color_printer *p3;
    struct color_printer *p4;

    struct printer_i **p;

    p1 = plain_printer_new(">>> ");
    p2 = plain_printer_new("*** ");
    p3 = color_printer_new("\033[31;47m");
    p4 = color_printer_new("\033[31;42m");

    p = (struct printer_i **)p1;
    // printf("p:%p,&p:%p,&p1:%p,p1:%p\n", p, &p, &p1, p1);
    (*p)->print(p, "hello from p1\n");

    p = (struct printer_i **)p2;
    (*p)->print(p, "hello from p2\n");

    p = (struct printer_i **)p3;
    (*p)->print(p, "hello from p3\n");

    p = (struct printer_i **)p4;
    (*p)->print(p, "hello from p4\n");

    printf("\n\n");
    plain_printer_cleanup(p1);
    plain_printer_cleanup(p2);
    color_printer_cleanup(p3);
    color_printer_cleanup(p4);
    return 0;
}