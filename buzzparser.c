#include "buzzparser.h"
#include <stdio.h>
#include <stdlib.h>

/****************************************/
/****************************************/

static int PARSE_ERROR  =  0;
static int PARSE_OK     =  1;
static int TYPE_BASIC   = -1;
static int TYPE_CLOSURE = -2;
static int TYPE_TABLE   = -3;

/****************************************/
/****************************************/

#define DEBUG(MSG, ...) fprintf(stderr, "[DEBUG] " MSG, ##__VA_ARGS__);
#define TODO (MSG, ...) fprintf(stderr, "[TODO] " MSG, ##__VA_ARGS__);

/****************************************/
/****************************************/

struct sym_s {
   /* Position of the symbol in the activation record (-1 for unknown) */
   int64_t pos;
   /* Symbol type
    * Can be any of the TYPE_* declarations
    * or a value >0 which is an offset in the local variable list
    */
   int type;
   /* 1 if the symbol is global, 0 if local */
   int global;
};

struct sym_s* sym_lookup(const char* sym,
                         buzzdarray_t symstack) {
   struct sym_s* symdata = NULL;
   /* Go through the symbol tables, from the top to the bottom */
   for(int64_t i = buzzdarray_size(symstack)-1; i >= 0; --i) {
      /* Get symbol table */
      buzzdict_t st = buzzdarray_get(symstack, i, buzzdict_t);
      /* Look for the symbol - if found, return immediately */
      symdata = buzzdict_get(st, &sym, struct sym_s);
      if(symdata) return symdata;
   }
   /* Symbol not found */
   return NULL;
}

#define sym_add(SYM) {                                     \
      char* key = strdup(SYM);                            \
      struct sym_s symdata = {                            \
         .pos  = (buzzdict_size(par->syms)),              \
         .type = TYPE_BASIC,                              \
         .global = (buzzdarray_size(par->symstack) == 1)  \
      };                                                  \
      buzzdict_set(par->syms, &key, &symdata);            \
   }

void sym_destroy(const void* key,
                void* data,
                void* params) {
   free(*(char**)key);
}

#define SYMT_BUCKETS 100

#define symt_push() par->syms = buzzdict_new(SYMT_BUCKETS, sizeof(char*), sizeof(struct sym_s), buzzdict_strkeyhash, buzzdict_strkeycmp, sym_destroy); buzzdarray_push(par->symstack, &(par->syms));

#define symt_pop() buzzdarray_pop(par->symstack); par->syms = buzzdarray_last(par->symstack, buzzdict_t);

void symt_destroy(uint32_t pos, void* data, void* params) {
   buzzdict_destroy((buzzdict_t*)data);
}

struct idrefinfo_s {
   /* When < 0 correspond to TYPE_*; when >=0 corresponds to offset in variable list */
   int info;
   /* For info >=0, says whether the idref is for local or global variable */
   int global;
};

/****************************************/
/****************************************/

#define LABELREF "@__label_"

/*
 * A chunk of code
 * This can be either
 * - the code in the global scope
 * - a function
 * - a lambda (anonymous function)
 */
struct chunk_s {
   /* The label for this chunk */
   uint32_t label;
   /* The code for this chunk */
   char* code;
   /* not-NULL if a symbol must be registered (function), NULL if not (lambda) */
   struct sym_s* sym;
   /* The code size */
   size_t csize;
   /* The buffer capacity */
   size_t ccap;
};
typedef struct chunk_s* chunk_t;

chunk_t chunk_new(uint32_t label, struct sym_s* sym) {
   chunk_t c = (chunk_t)malloc(sizeof(struct chunk_s));
   c->label = label;
   c->csize = 0;
   c->ccap = 10;
   c->code = (char*)malloc(c->ccap);
   c->sym = sym;
   return c;
}

void chunk_destroy(uint32_t pos, void* data, void* params) {
   chunk_t* c = (chunk_t*)data;
   free((*c)->code);
   free(*c);
   *c = NULL;
}

void chunk_addcode(chunk_t c, char* code) {
   /* Get length of the code */
   size_t l = strlen(code);
   /* Resize the code buffer */
   if(c->csize + l >= c->ccap) {
      do { c->ccap *= 2; } while(c->csize + l >= c->ccap);
      c->code = realloc(c->code, c->ccap);
   }
   /* Copy the code */
   strncpy(c->code + c->csize, code, l);
   /* Update size */
   c->csize += l;
}

void chunk_finalize(chunk_t c) {
   /* Shrink the memory allocation */
   c->ccap = c->csize + 1;
   c->code = realloc(c->code, c->ccap);   
   /* Add a \0 at the end */
   c->code[c->csize] = 0;
}

#define chunk_append(...) {                         \
      char* str;                                    \
      asprintf(&str, ##__VA_ARGS__);                \
      chunk_addcode(par->chunk, str);               \
      free(str);                                    \
   }

void chunk_register(uint32_t pos, void* data, void* params) {
   /* Cast params */
   chunk_t c = *(chunk_t*)data;
   FILE* f = (FILE*)params;
   /* Print registration code */
   if(c->sym) {
      fprintf(f, "\tpushi " LABELREF "%u\n", c->label);
      fprintf(f, "\tpushcn\n");
      if(c->sym->global) fprintf(f, "\tgstore %lld\n", c->sym->pos);
      else               fprintf(f, "\tlstore %lld\n", c->sym->pos);
   }
}

void chunk_print(uint32_t pos, void* data, void* params) {
   /* Cast params */
   chunk_t c = *(chunk_t*)data;
   FILE* f = (FILE*)params;
   /* Print the label */
   fprintf(f, "\n" LABELREF "%u\n", c->label);
   /* Print the code */
   fprintf(f, "%s", c->code);
}

#define chunk_push(SYM)                                        \
   chunk_t oldc = par->chunk;                                  \
   par->chunk = chunk_new(par->labels, (SYM));                 \
   buzzdarray_push(par->chunks, &par->chunk);                  \
   ++(par->labels);

#define chunk_pop() chunk_finalize(par->chunk); par->chunk = oldc;

/****************************************/
/****************************************/

#define fetchtok()                                              \
   buzzlex_destroytok(&par->tok);                               \
   par->tok = buzzlex_nexttok(par->lex);                        \
   if(!par->tok) {                                              \
      fprintf(stderr,                                           \
              "%s: Syntax error: expected token, found EOF\n",  \
              par->lex->fname);                                 \
      return PARSE_ERROR;                                       \
   }                                                            \

int match(buzzparser_t par,
          buzztok_type_e type) {
   if(!par->tok) {
      fprintf(stderr,
              "%s: Syntax error: expected token, found EOFn",
              par->lex->fname);
      return PARSE_ERROR;
   }
   if(par->tok->type != type) {
      fprintf(stderr,
              "%s:%llu:%llu: Syntax error: expected %s, found %s\n",
              par->lex->fname,
              par->tok->line,
              par->tok->col,
              buzztok_desc[type],
              buzztok_desc[par->tok->type]);
      return PARSE_ERROR;
   }
   else {
      DEBUG("%s:%llu:%llu: Matched %s\n",
            par->lex->fname,
            par->tok->line,
            par->tok->col,
            buzztok_desc[type]);
      return PARSE_OK;
   }
}

#define tokmatch(tok) if(!match(par, tok)) return PARSE_ERROR;

/****************************************/
/****************************************/

int parse_script(buzzparser_t par);

int parse_statlist(buzzparser_t par);
int parse_stat(buzzparser_t par);

int parse_block(buzzparser_t par);

int parse_fun(buzzparser_t par);
int parse_if(buzzparser_t par);
int parse_for(buzzparser_t par);
int parse_while(buzzparser_t par);

int parse_conditionlist(buzzparser_t par,
                        int* numargs);
int parse_condition(buzzparser_t par);
int parse_comparison(buzzparser_t par);

int parse_expression(buzzparser_t par);
int parse_product(buzzparser_t par);
int parse_modulo(buzzparser_t par);
int parse_power(buzzparser_t par);
int parse_powerrest(buzzparser_t par);
int parse_operand(buzzparser_t par);

int parse_command(buzzparser_t par);

int parse_idlist(buzzparser_t par);
int parse_idreflist(buzzparser_t par);
int parse_idref(buzzparser_t par,
                int lvalue,
                struct idrefinfo_s* idrefinfo);

int parse_lambda(buzzparser_t par);

/****************************************/
/****************************************/

int parse_script(buzzparser_t par) {
   /* Fetch the first token */
   par->tok = buzzlex_nexttok(par->lex);
   if(!par->tok) {
      DEBUG("%s: Empty file\n",
            par->lex->fname);
      return PARSE_ERROR;
   }
   /* Add a symbol table */
   symt_push();
   /* Add the first chunk for the global scope */
   chunk_push(0);
   /* Parse the statements */
   if(!parse_statlist(par)) return PARSE_ERROR;
   /* Finalize the output */
   chunk_append("\n@__exitpoint\n");
   chunk_append("\tdone\n");
   return PARSE_OK;
}

/****************************************/
/****************************************/

int parse_statlist(buzzparser_t par) {
   DEBUG("Parsing statement list start\n");
   if(!parse_stat(par)) return PARSE_ERROR;
   while(par->tok && par->tok->type == BUZZTOK_STATEND) {
      buzzlex_destroytok(&par->tok);
      par->tok = buzzlex_nexttok(par->lex);
      if(par->tok && !parse_stat(par)) return PARSE_ERROR;
   }
   if(!par->tok || par->tok->type == BUZZTOK_BLOCKCLOSE) {
      DEBUG("Statement list end\n");
      return PARSE_OK;
   }
   else {
      return PARSE_ERROR;
   }
}

int parse_stat(buzzparser_t par) {
   DEBUG("Parsing statement\n");
   if(par->tok->type == BUZZTOK_STATEND || par->tok->type == BUZZTOK_BLOCKCLOSE) {
      DEBUG("Statement end\n");
      return PARSE_OK;
   }
   if(par->tok->type == BUZZTOK_FUN)
      return parse_fun(par);
   if(par->tok->type == BUZZTOK_IF)
      return parse_if(par);
   if(par->tok->type == BUZZTOK_FOR)
      return parse_for(par);
   if(par->tok->type == BUZZTOK_WHILE)
      return parse_while(par);
   return parse_command(par);
}

/****************************************/
/****************************************/

/* Data passed along when searching for variables to erase */
struct symdeldata_s {
   /* List of variables to erase, initially empty */
   buzzdarray_t dellist;
   /* Maximum position a variable can have - those with higher pos are deleted */
   uint32_t maxpos;
};

void buzzparser_symtodel(const void* key, void* data, void* params) {
   struct symdeldata_s* symdeldata = (struct symdeldata_s*)params;
   if(((struct sym_s*)data)->pos >= symdeldata->maxpos) {
      buzzdarray_push(symdeldata->dellist, (char**)key);
   }
}

void buzzparser_symdel(uint32_t pos, void* data, void* params) {
   DEBUG("Deleted local var '%s'\n", *(char**)data);
   buzzdict_remove((buzzdict_t)params, (char**)data);
}

int parse_block(buzzparser_t par) {
   DEBUG("Parsing block start\n");
   tokmatch(BUZZTOK_BLOCKOPEN);
   fetchtok();
   if(par->tok->type == BUZZTOK_BLOCKCLOSE) {
      DEBUG("Block end\n");
      fetchtok();
      return PARSE_OK;
   }
   else {
      /* Save the current number of variables */
      uint32_t numvars = buzzdict_size(par->syms);
      if(parse_statlist(par)) {
         tokmatch(BUZZTOK_BLOCKCLOSE);
         fetchtok();
         /* Remove the variables defined in this block */
         struct symdeldata_s symdeldata = {
            .dellist = buzzdarray_new(1, sizeof(char*), NULL),
            .maxpos = numvars
         };
         buzzdict_foreach(par->syms, buzzparser_symtodel, &symdeldata);
         buzzdarray_foreach(symdeldata.dellist, buzzparser_symdel, par->syms);
         buzzdarray_destroy(&(symdeldata.dellist));
         DEBUG("Block end\n");
         return PARSE_OK;
      }
      else {
         return PARSE_ERROR;
      }
   }
}

/****************************************/
/****************************************/

int parse_fun(buzzparser_t par) {
   DEBUG("Parsing function definition\n");
   tokmatch(BUZZTOK_FUN);
   fetchtok();
   tokmatch(BUZZTOK_ID);
   /* Add a symbol for this function */
   sym_add(par->tok->value);
   /* Make a new chunk for this function and get the associated symbol */
   chunk_push(sym_lookup(par->tok->value, par->symstack));
   fetchtok();
   tokmatch(BUZZTOK_PAROPEN);
   fetchtok();
   symt_push();
   if(!parse_idlist(par)) return PARSE_ERROR;
   tokmatch(BUZZTOK_PARCLOSE);
   fetchtok();
   if(!parse_block(par)) return PARSE_ERROR;
   symt_pop();
   chunk_pop();
   return PARSE_OK;
}

int parse_if(buzzparser_t par) {
   DEBUG("Parsing if start\n");
   /* Save labels for else branch and for if end */
   uint32_t lab1 = par->labels;
   uint32_t lab2 = par->labels + 1;
   par->labels += 2;
   /* Parse tokens */
   tokmatch(BUZZTOK_IF);
   fetchtok();
   tokmatch(BUZZTOK_PAROPEN);
   fetchtok();
   if(!parse_condition(par)) return PARSE_ERROR;
   tokmatch(BUZZTOK_PARCLOSE);
   fetchtok();
   /* True branch follows condition; false branch follows true one */
   /* Jamp to label 1 if the condition is false */
   /* Label 1 is either if end (in case of no else branch) or else branch */
   chunk_append("\tjumpz " LABELREF "%u\n", lab1);
   if(!parse_block(par)) return PARSE_ERROR;
   /* Eat away the newlines, if any */
   while(par->tok->type == BUZZTOK_STATEND && !par->tok->value) { fetchtok(); }
   if(par->tok->type == BUZZTOK_ELSE) {
      DEBUG("Else found\n");
      fetchtok();
      /* Make true branch jump to label 2 => if end */
      chunk_append("\tjump " LABELREF "%u\n", lab2);
      /* Mark this place as label 1 and keep parsing */
      chunk_append(LABELREF "%u\n", lab1);
      if(!parse_block(par)) return PARSE_ERROR;
      /* Mark the if end as label 2 */
      chunk_append(LABELREF "%u\n", lab2);
   }
   else {
      /* Mark the if end as label 1 */
      chunk_append(LABELREF "%u\n", lab1);
   }
   DEBUG("If end\n");
   return PARSE_OK;
}

/****************************************/
/****************************************/

int parse_for(buzzparser_t par) {
   DEBUG("Parsing for\n");
   tokmatch(BUZZTOK_FOR);
   fetchtok();
   tokmatch(BUZZTOK_PAROPEN);
   fetchtok();
   struct idrefinfo_s idrefinfo;
   if(!parse_idref(par, 1, &idrefinfo)) return PARSE_ERROR;
   tokmatch(BUZZTOK_ASSIGN);
   fetchtok();
   if(!parse_expression(par)) return PARSE_ERROR;
   tokmatch(BUZZTOK_LISTSEP);
   fetchtok();
   if(!parse_condition(par)) return PARSE_ERROR;
   tokmatch(BUZZTOK_LISTSEP);
   fetchtok();
   if(!parse_idref(par, 1, &idrefinfo)) return PARSE_ERROR;
   tokmatch(BUZZTOK_ASSIGN);
   fetchtok();
   if(!parse_expression(par)) return PARSE_ERROR;
   tokmatch(BUZZTOK_PARCLOSE);
   fetchtok();
   return parse_block(par);
}

/****************************************/
/****************************************/

int parse_while(buzzparser_t par) {
   DEBUG("Parsing while\n");
   tokmatch(BUZZTOK_WHILE);
   fetchtok();
   tokmatch(BUZZTOK_PAROPEN);
   fetchtok();
   if(!parse_condition(par)) return PARSE_ERROR;
   tokmatch(BUZZTOK_PARCLOSE);
   fetchtok();
   return parse_block(par);
}

/****************************************/
/****************************************/

int parse_conditionlist(buzzparser_t par,
                        int* numargs) {
   DEBUG("Parsing condition list start\n");
   *numargs = 0;
   if(par->tok->type == BUZZTOK_PARCLOSE) return PARSE_OK;
   if(!parse_condition(par)) return PARSE_ERROR;
   ++(*numargs);
   while(par->tok->type == BUZZTOK_LISTSEP) {
      fetchtok();
      if(!parse_condition(par)) return PARSE_ERROR;
      ++(*numargs);
   }
   DEBUG("Condition list end\n");
   return PARSE_OK;
}

int parse_condition(buzzparser_t par) {
   DEBUG("Parsing condition start\n");
   if(!parse_comparison(par)) return PARSE_ERROR;
   while(par->tok->type == BUZZTOK_ANDOR) {
      char op[4];
      strcpy(op, par->tok->value);
      fetchtok();
      if(!parse_comparison(par)) return PARSE_ERROR;
      chunk_append("\t%s\n", op);
   }
   DEBUG("Condition end\n");
   return PARSE_OK;
}

int parse_comparison(buzzparser_t par) {
   if(par->tok->type == BUZZTOK_PAROPEN) {
      DEBUG("Parsing (condition) start\n");
      fetchtok();
      if(!parse_condition(par)) return PARSE_ERROR;
      tokmatch(BUZZTOK_PARCLOSE);
      fetchtok();
      DEBUG("(condition) end\n");
      return PARSE_OK;
   }
   else if(par->tok->type == BUZZTOK_NOT) {
      DEBUG("Parsing NOT condition start\n");
      fetchtok();
      if(!parse_comparison(par)) return PARSE_ERROR;
      chunk_append("\tnot\n");
      DEBUG("NOT condition end\n");
      return PARSE_OK;
   }
   else {
      DEBUG("Parsing comparison condition start\n");
      if(!parse_expression(par)) return PARSE_ERROR;
      if(par->tok->type == BUZZTOK_CMP) {
         char op[4];
         if     (strcmp(par->tok->value, "==") == 0) strcpy(op, "eq");
         else if(strcmp(par->tok->value, "!=") == 0) strcpy(op, "neq");
         else if(strcmp(par->tok->value, "<")  == 0) strcpy(op, "lt");
         else if(strcmp(par->tok->value, "<=") == 0) strcpy(op, "lte");
         else if(strcmp(par->tok->value, ">")  == 0) strcpy(op, "gt");
         else if(strcmp(par->tok->value, ">=") == 0) strcpy(op, "gte");
         fetchtok();
         if(!parse_expression(par)) return PARSE_ERROR;
         chunk_append("\t%s\n", op);
      }
      DEBUG("Parsing comparison condition end\n");
      return PARSE_OK;
   }
}

/****************************************/
/****************************************/

int parse_expression(buzzparser_t par) {
   DEBUG("Parsing expression start\n");
   if(par->tok->type == BUZZTOK_BLOCKOPEN) {
      fetchtok();
      tokmatch(BUZZTOK_BLOCKCLOSE);
      fetchtok();
      chunk_append("\tpusht\n");
      DEBUG("Expression end\n");
      return PARSE_OK;
   }
   if(!parse_product(par)) return PARSE_ERROR;
   while(par->tok->type == BUZZTOK_ADDSUB) {
      DEBUG("Parsing +- expression\n");
      char op = par->tok->value[0];
      fetchtok();
      if(!parse_product(par)) return PARSE_ERROR;
      if     (op == '+') { chunk_append("\tadd\n"); }
      else if(op == '-') { chunk_append("\tsub\n"); }
   }
   DEBUG("Expression end\n");
   return PARSE_OK;
}

int parse_product(buzzparser_t par) {
   DEBUG("Parsing product start\n");
   if(!parse_modulo(par)) return PARSE_ERROR;
   while(par->tok->type == BUZZTOK_MULDIV) {
      DEBUG("Parsing */ product\n");
      char op = par->tok->value[0];
      fetchtok();
      if(!parse_modulo(par)) return PARSE_ERROR;
      if(op == '*') {
         chunk_append("\tmul\n");
      }
      else if(op == '/') {
         chunk_append("\tdiv\n");
      }
   }
   DEBUG("Product end\n");
   return PARSE_OK;
}

int parse_modulo(buzzparser_t par) {
   DEBUG("Parsing modulo start\n");
   if(!parse_power(par)) return PARSE_ERROR;
   while(par->tok->type == BUZZTOK_MOD) {
      DEBUG("Parsing modulo\n");
      fetchtok();
      if(!parse_power(par)) return PARSE_ERROR;
      chunk_append("\tmod\n");
   }
   DEBUG("Modulo end\n");
   return PARSE_OK;
}

int parse_power(buzzparser_t par) {
   DEBUG("Parsing power start\n");
   return parse_operand(par) && parse_powerrest(par);
}

int parse_powerrest(buzzparser_t par) {
   DEBUG("Parsing power rest\n");
   if(par->tok->type == BUZZTOK_POW) {
      DEBUG("Parsing power\n");
      fetchtok();
      if(!parse_power(par)) return PARSE_ERROR;
      chunk_append("\tpow\n");
   }
   DEBUG("End power\n");
   return PARSE_OK;
}

int parse_operand(buzzparser_t par) {
   DEBUG("Parsing operand\n");
   if(par->tok->type == BUZZTOK_FUN) {
      DEBUG("Operand is lambda\n");
      chunk_append("\tpushi " LABELREF "%u\n", par->labels);
      chunk_append("\tpushcn\n");
      if(!parse_lambda(par)) return PARSE_ERROR;
      return PARSE_OK;
   }
   else if(par->tok->type == BUZZTOK_BOOL) {
      DEBUG("Operand is token true/false\n");
      fetchtok();
      return PARSE_OK;
   }
   else if(par->tok->type == BUZZTOK_CONST) {
      DEBUG("Operand is numeric constant\n");
      if(strchr(par->tok->value, '.')) {
         /* Floating-point constant */
         chunk_append("\tpushf %s\n", par->tok->value);
      }
      else {
         /* Integer constant */
         chunk_append("\tpushi %s\n", par->tok->value);
      }
      fetchtok();
      return PARSE_OK;
   }
   else if(par->tok->type == BUZZTOK_STRING) {
      DEBUG("Operand is string\n");
      fetchtok();
      return PARSE_OK;
   }
   else if(par->tok->type == BUZZTOK_PAROPEN) {
      DEBUG("Operand is (expression)\n");
      fetchtok();
      if(!parse_expression(par)) return PARSE_ERROR;
      tokmatch(BUZZTOK_PARCLOSE);
      fetchtok();
      DEBUG("(expression) end\n");
      return PARSE_OK;
   }
   else if(par->tok->type == BUZZTOK_ADDSUB) {
      char op = par->tok->value[0];
      fetchtok();
      if(par->tok->type == BUZZTOK_CONST) {
         DEBUG("Operand is signed +- constant\n");
         if(strchr(par->tok->value, '.')) {
            /* Floating-point constant */
            chunk_append("\tpushf %c%s\n", op, par->tok->value);
         }
         else {
            /* Integer constant */
            chunk_append("\tpushi %c%s\n", op, par->tok->value);
         }
         fetchtok();
         return PARSE_OK;
      }
      else {
         DEBUG("Operand is signed +-\n");
         if(!parse_power(par)) return PARSE_ERROR;
         if(op == '-') chunk_append("\tunm\n");
         DEBUG("Signed operand +- end\n");
         return PARSE_OK;
      }
   }
   DEBUG("Operand is idref\n");
   struct idrefinfo_s  idrefinfo;
   return parse_idref(par, 0, &idrefinfo);
}

/****************************************/
/****************************************/

int parse_command(buzzparser_t par) {
   DEBUG("Parsing command start\n");
   if(par->tok->type == BUZZTOK_RETURN) {
      /* Return statement */
      fetchtok();
      if(par->tok->type == BUZZTOK_STATEND ||
         par->tok->type == BUZZTOK_BLOCKCLOSE) {
         chunk_append("\tret0\n");
      }
      else {
         if(!parse_expression(par)) return PARSE_ERROR;
         chunk_append("\tret1\n");         
      }
      return PARSE_OK;
   }
   else {
      /* Function call or assignment, both begin with an id */
      struct idrefinfo_s idrefinfo;
      if(!parse_idref(par, 1, &idrefinfo)) return PARSE_ERROR;
      if(par->tok->type == BUZZTOK_ASSIGN) {
         if(idrefinfo.info == TYPE_CLOSURE) {
            fprintf(stderr,
                    "%s:%llu:%llu: Syntax error: can't have a function call as lvalue\n",
                    par->lex->fname,
                    par->tok->line,
                    par->tok->col);
            return PARSE_ERROR;
         }
         DEBUG("Parsing assignment\n");
         fetchtok();
         if(!parse_expression(par)) return PARSE_ERROR;
         if(idrefinfo.info >= 0) {
            if(idrefinfo.global) { chunk_append("\tgstore %d\n", idrefinfo.info); }
            else                 { chunk_append("\tlstore %d\n", idrefinfo.info); }
         }
         else if(idrefinfo.info == TYPE_TABLE)   { chunk_append("\ttset\n"); }
         else if(idrefinfo.info == TYPE_CLOSURE) { chunk_append("\tcallc\n"); }
         DEBUG("Assignment statement end\n");
         return PARSE_OK;
      }
      else if(idrefinfo.info == TYPE_CLOSURE) {
         DEBUG("Function call\n");
         DEBUG("Statement end\n");
         return PARSE_OK;
      }
      fprintf(stderr,
              "%s:%llu:%llu: Syntax error: expected function call or assignment\n",
              par->lex->fname,
              par->tok->line,
              par->tok->col);
      return PARSE_ERROR;
   }
}

/****************************************/
/****************************************/

int parse_idlist(buzzparser_t par) {
   DEBUG("Parsing idlist start\n");
   /* Empty list */
   if(par->tok->type == BUZZTOK_PARCLOSE) {
      DEBUG("Idlist end\n");
      return PARSE_OK;
   }
   /* Match an id for the first argument */
   tokmatch(BUZZTOK_ID);
   /* Look for the argument symbol in the context
    * If a symbol is found and is not global,
    * do not add a new symbol; otherwise do
    * This allows lambdas and nested functions to
    * reuse the parameters and local variables of
    * the parent
    */
   struct sym_s* sym = sym_lookup(par->tok->value, par->symstack);
   if(!sym || sym->global) { sym_add(par->tok->value); }
   fetchtok();
   /* Match rest of the argument list */
   while(par->tok->type == BUZZTOK_LISTSEP) {
      fetchtok();
      tokmatch(BUZZTOK_ID);
      struct sym_s* sym = sym_lookup(par->tok->value, par->symstack);
      if(!sym || sym->global) { sym_add(par->tok->value); }
      fetchtok();
   }
   DEBUG("Idlist end\n");
   return PARSE_OK;
}

int parse_idreflist(buzzparser_t par) {
   DEBUG("Parsing idreflist start\n");
   if(par->tok->type == BUZZTOK_PARCLOSE) {
      DEBUG("idreflist end\n");
      return PARSE_OK;
   }
   struct idrefinfo_s idrefinfo;
   if(!parse_idref(par, 0, &idrefinfo)) return PARSE_ERROR;
   while(par->tok->type == BUZZTOK_LISTSEP) {
      DEBUG("Parsing next idreflist item\n");
      fetchtok();
      if(!parse_idref(par, 0, &idrefinfo)) return PARSE_ERROR;
   }
   if(par->tok && par->tok->type == BUZZTOK_PARCLOSE) {
      DEBUG("Idreflist end\n");
      return PARSE_OK;
   }
   fprintf(stderr,
           "%s:%llu:%llu: Syntax error: expected , or ), found %s\n",
           par->lex->fname,
           par->tok->line,
           par->tok->col,
           buzztok_desc[par->tok->type]);
   return PARSE_ERROR;
}

int parse_idref(buzzparser_t par,
                int lvalue,
                struct idrefinfo_s* idrefinfo) {
   DEBUG("Parsing idref start\n");
   /* Start with an id */
   tokmatch(BUZZTOK_ID);
   /* Look it up in the symbol table */
   struct sym_s* s = sym_lookup(par->tok->value, par->symstack);
   if(!s) {
      /* Symbol not found */
      /* If the symbol is not an lvalue or
       * is used as a base for a structured type,
       * error */
      if(!lvalue ||
         (par->tok->type == BUZZTOK_DOT ||
          par->tok->type == BUZZTOK_IDXOPEN ||
          par->tok->type == BUZZTOK_PAROPEN)) {
         /* No, error */
         fprintf(stderr,
                 "%s:%llu:%llu: Undefined symbol %s\n",
                 par->lex->fname,
                 par->tok->line,
                 par->tok->col,
                 par->tok->value);
         return PARSE_ERROR;
      }
      /* Otherwise, set the idrefinfo */
      DEBUG("Adding unknown idref %s\n", par->tok->value);
      idrefinfo->info = buzzdict_size(par->syms);
      idrefinfo->global = (buzzdarray_size(par->symstack) == 1);
      sym_add(par->tok->value);
      fetchtok();
      /* Nothing else to do */
      return PARSE_OK;
   }
   /* Symbol found, save its info */
   DEBUG("Found idref %s\n", par->tok->value);
   idrefinfo->info = s->pos;
   idrefinfo->global = s->global;
   /* Go on parsing the reference */
   fetchtok();
   while(par->tok->type == BUZZTOK_DOT ||
         par->tok->type == BUZZTOK_IDXOPEN ||
         par->tok->type == BUZZTOK_PAROPEN) {
      /* Take care of structured types */
      if(idrefinfo->info >= 0) {
         if(s->global) { chunk_append("\tgload %lld\n", s->pos); }
         else          { chunk_append("\tlload %lld\n", s->pos); }
      }
      else if(idrefinfo->info == TYPE_TABLE)   { chunk_append("\ttget\n"); }
      else if(idrefinfo->info == TYPE_CLOSURE) { chunk_append("\tcallc\n"); }
      if(par->tok->type == BUZZTOK_DOT) {
         DEBUG("Parsing idref.idref\n");
         idrefinfo->info = TYPE_TABLE;
         fetchtok();
         tokmatch(BUZZTOK_ID);
         chunk_append("\tpushs %s\n", par->tok->value);
         fetchtok();
      }
      if(par->tok->type == BUZZTOK_IDXOPEN) {
         idrefinfo->info = TYPE_TABLE;
         DEBUG("Parsing idref[expression]\n");
         fetchtok();
         if(!parse_expression(par)) return PARSE_ERROR;
         tokmatch(BUZZTOK_IDXCLOSE);
         fetchtok();
      }
      else if(par->tok->type == BUZZTOK_PAROPEN) {
         idrefinfo->info = TYPE_CLOSURE;
         DEBUG("Parsing function call\n");
         fetchtok();
         int numargs;
         if(!parse_conditionlist(par, &numargs)) return PARSE_ERROR;
         tokmatch(BUZZTOK_PARCLOSE);
         fetchtok();
         chunk_append("\tpushi %d\n", numargs);
      }
   }
   if(!lvalue) {
      if(idrefinfo->info >= 0) {
         if(s->global) { chunk_append("\tgload %lld\n", s->pos); }
         else          { chunk_append("\tlload %lld\n", s->pos); }
      }
      else if(idrefinfo->info == TYPE_TABLE) {
         chunk_append("\ttget\n");
      }
      else if(idrefinfo->info == TYPE_CLOSURE) {
         chunk_append("\tcallc\n");
      }
   }
   DEBUG("Idref end\n");
   return PARSE_OK;
}

/****************************************/
/****************************************/

int parse_lambda(buzzparser_t par) {
   DEBUG("Parsing lambda\n");
   tokmatch(BUZZTOK_FUN);
   fetchtok();
   /* Make a new chunk for this function and get the associated symbol */
   chunk_push(NULL);
   tokmatch(BUZZTOK_PAROPEN);
   fetchtok();
   if(!parse_idlist(par)) return PARSE_ERROR;
   tokmatch(BUZZTOK_PARCLOSE);
   fetchtok();
   if(!parse_block(par)) return PARSE_ERROR;
   chunk_pop();
   DEBUG("Lambda done\n");
   return PARSE_OK;
}

/****************************************/
/****************************************/

buzzparser_t buzzparser_new(const char* fscript,
                            const char* fasm) {
   /* Create parser state */
   buzzparser_t par = (buzzparser_t)malloc(sizeof(struct buzzparser_s));
   /* Create lexer */
   par->lex = buzzlex_new(fscript);
   if(!par->lex) {
      free(par);
      return NULL;
   }
   /* Copy string */
   par->asmfn = strdup(fasm);
   /* Open file */
   par->asmstream = fopen(par->asmfn, "w");
   if(!par->asmstream) {
      perror(par->asmfn);
      free(par->asmfn);
      free(par);
      return NULL;
   }
   /* Initialize label counter */
   par->labels = 0;
   /* Initialize chunk list */
   par->chunks = buzzdarray_new(1, sizeof(chunk_t), chunk_destroy);
   /* Initialize symbol table stack */
   par->symstack = buzzdarray_new(10, sizeof(buzzdict_t), symt_destroy);
   par->syms = NULL;
   /* Return parser state */
   return par;
}

/****************************************/
/****************************************/

void buzzparser_destroy(buzzparser_t* par) {
   buzzdarray_destroy(&((*par)->chunks));
   buzzdarray_destroy(&((*par)->symstack));
   free((*par)->asmfn);
   fclose((*par)->asmstream);
   buzzlex_destroy(&((*par)->lex));
   if((*par)->tok) buzzlex_destroytok(&((*par)->tok));
   free(*par);
   *par = NULL;
}

/****************************************/
/****************************************/

int buzzparser_parse(buzzparser_t par) {
   /* Parse the script */
   if(!parse_script(par)) return PARSE_ERROR;
   /* Write to file */
   buzzdarray_foreach(par->chunks, chunk_register, par->asmstream);
   buzzdarray_foreach(par->chunks, chunk_print,    par->asmstream);
   return PARSE_OK;
}

/****************************************/
/****************************************/