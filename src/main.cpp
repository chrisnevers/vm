#include <iostream>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "ast.h"
#include "gc.h"

using namespace std;

bool debug = true;

// Temp string for reading memory
char* tmp = (char*) malloc (8 * sizeof (char));
char* mm;       // Mapped memory location
int fd;         // Bytecode program file descriptor
int fs;         // Bytecode program file size

// Initialize garbage collector heap
char heap[heap_size] = {};
size_t heap_max = heap_size - 1;

char* from_space_begin = heap;
char* from_space_end = heap + (heap_size / 2) - 1;
char* to_space_begin = heap + (heap_size / 2);
char* to_space_end = heap + heap_size;
char* free_ptr = from_space_begin;

size_t pf = 0;
M* pc = 0;
E* pe;
K* pk;

void cek () {
    pe = e_mt();
    pk = k_ret();

    while (true) {
        M* m = pc;
        size_t tag = m->tag;

        display_state (pc, pe, pk);

        switch (tag) {
            case TMLam: {
                MClo* clo   = (MClo*) malloc1 (sizeof(MClo));
                clo->m      = mk_m (TMClo);
                clo->ex     = pc;
                clo->env    = pe;
                pc          = (M*) clo;
                pe          = e_mt ();
                break;
            }
            case TMApp: {
                KFn* node   = (KFn*) malloc1 (sizeof(KFn));
                MApp* app   = (MApp*) pc;
                node->k     = mk_k (TKFn);
                node->m     = deref(app->arg);
                node->e     = pe;
                node->ok    = pk;
                pc = deref(app->fn);
                pk = (K*) node;
                break;
            }
            case TMIf: {
                KIf* node   = (KIf*) malloc1 (sizeof(KIf));
                MIf* i      = (MIf*) pc;
                node->k     = mk_k (TKIf);
                node->ok    = pk;
                node->t     = deref(i->thn);
                node->e     = deref(i->els);
                pc = deref(i->cnd);
                pk = (K*) node;
                break;
            }
            case TMVar: {
                MVar* var   = (MVar*) pc;
                E* e        = pe;
                M* old_pc   = pc;
                while (e->tag != TEMt) {
                    EClo* clo = (EClo*) e;
                    if (var->id == clo->id) {
                        pc = deref(clo->val);
                        break;
                    } else {
                        e = clo->nxt;
                    }
                }
                if (pc == old_pc) {
                    throw logic_error
                        ("Unbound variable: v" + to_string (var->id));
                }
                break;
            }
            case TMTru:
            case TMFals:
            case TMPair:
            case TMNul:
            case TMClo:
            case TMNum: {
                size_t k_tag = pk->tag;
                switch (k_tag) {
                    case TKRet: {
                        if (is_v (tag)) {
                            display_m (tag, pc);
                            return;
                        }
                        break;
                    }
                    case TKFn: {
                        KArg* node  = (KArg*) malloc1 (sizeof(KArg));
                        KFn* fn     = (KFn*) pk;
                        node->k     = mk_k (TKArg);
                        node->m     = pc;
                        node->ok    = fn->ok;
                        pk = (K*) node;
                        pc = deref(fn->m);
                        pe = fn->e;
                        break;
                    }
                    case TKIf: {
                        KIf* node = (KIf*) pk;
                        switch (pc->tag) {
                            case TMTru:  { pc = node->t; break; }
                            case TMFals: { pc = node->e; break; }
                        }
                        pk = node->ok;
                        break;
                    }
                    case TKArg: {
                        KArg* ar = (KArg*) pk;
                        if (ar->m->tag == TMClo) {
                            EClo* node  = (EClo*) malloc1 (sizeof(EClo));
                            ar = (KArg*) pk;
                            MClo* clo   = (MClo*) ar->m;
                            MLam* lam   = (MLam*) clo->ex;
                            E* env = (clo->env->tag == TEMt)
                                ? e_mt () : clo->env;
                            node->e     = mk_e (TEClo);
                            node->id    = lam->id;
                            node->val   = pc;
                            node->nxt   = env;
                            pc = deref(lam->body);
                            pe = (E*) node;
                            pk = ar->ok;
                        } else {
                            throw logic_error ("Expected Closure in KArg");
                        }
                        break;
                    }
                    case TKOp1: {
                        KOp1* op = (KOp1*) pk;
                        switch (op->op) {
                            case TPNeg: {
                                MNum* val   = (MNum*) malloc1 (sizeof(MNum));
                                val->m      = mk_m (TMNum);
                                MNum* r     = (MNum*) deref(pc);
                                val->val    = - r->val;
                                pc = (M*) val;
                                break;
                            }
                            case TPFst: {
                                MPair* pair = (MPair*) pc;
                                M* val      = new_m(&pair->l);
                                pc = (M*) val;
                                break;
                            }
                            case TPSnd: {
                                MPair* pair = (MPair*) pc;
                                M* val      = new_m(&pair->r);
                                pc = (M*) val;
                                break;
                            }
                            default: {
                                throw logic_error ("Unknown operator");
                            }
                        };
                        // pe = e_mt ();
                        pk = op->ok;
                        break;
                    }
                    case TKOp2: {
                        KOp2** op = (KOp2**) &pk;
                        // Solve second value now
                        M* m = deref ((*op)->m);
                        if (m->tag != TMNul) {
                            KOp2* nk = (KOp2*) malloc1 (sizeof(KOp2));
                            nk->k   = (*op)->k;
                            nk->op  = (*op)->op;
                            nk->v   = pc;
                            nk->m   = m_nul ();
                            nk->ok  = (*op)->ok;
                            pc = deref((*op)->m);
                            pk = (K*) nk;
                        } else {
                            // Values have been computed
                            switch ((*op)->op) {
                                case TPAdd: {
                                    MNum* val = (MNum*) malloc1 (sizeof(MNum));
                                    val->m  = mk_m (TMNum);
                                    MNum* l = (MNum*) (*op)->v;
                                    MNum* r = (MNum*) pc;
                                    val->val = l->val + r->val;
                                    pc = (M*) val;
                                    break;
                                }
                                case TPSub: {
                                    MNum* val = (MNum*) malloc1 (sizeof(MNum));
                                    val->m  = mk_m (TMNum);
                                    MNum* l = (MNum*) (*op)->v;
                                    MNum* r = (MNum*) pc;
                                    val->val = l->val - r->val;
                                    pc = (M*) val;
                                    break;
                                }
                                case TPMkPair: {
                                    MPair* val = (MPair*) malloc1 (sizeof(MPair));
                                    val->m = mk_m (TMPair);
                                    val->l = new_m(&(*op)->v);
                                    val->r = new_m(&pc);
                                    pc = (M*) val;
                                    break;
                                }
                                case TPLt: {
                                    M* res = (M*) malloc1 (sizeof(M));
                                    MNum* l = (MNum*) (*op)->v;
                                    MNum* r = (MNum*) pc;
                                    res->tag = (l->val < r->val) ? TMTru : TMFals;
                                    pc = (M*) res;
                                    break;
                                }
                                case TPLtE: {
                                    M* res = (M*) malloc1 (sizeof(M));
                                    MNum* l = (MNum*) (*op)->v;
                                    MNum* r = (MNum*) pc;
                                    res->tag = (l->val <= r->val) ? TMTru : TMFals;
                                    pc = (M*) res;
                                    break;
                                }
                                case TPGt: {
                                    M* res = (M*) malloc1 (sizeof(M));
                                    MNum* l = (MNum*) (*op)->v;
                                    MNum* r = (MNum*) pc;
                                    res->tag = (l->val > r->val) ? TMTru : TMFals;
                                    pc = (M*) res;
                                    break;
                                }
                                case TPGtE: {
                                    M* res = (M*) malloc1 (sizeof(M));
                                    MNum* l = (MNum*) (*op)->v;
                                    MNum* r = (MNum*) pc;
                                    res->tag = (l->val >= r->val) ? TMTru : TMFals;
                                    pc = (M*) res;
                                    break;
                                }
                                case TPEq: {
                                    M* res = (M*) malloc1 (sizeof(M));
                                    MNum* l = (MNum*) (*op)->v;
                                    MNum* r = (MNum*) pc;
                                    res->tag = (l->val == r->val) ? TMTru : TMFals;
                                    pc = (M*) res;
                                    break;
                                }
                                default: {
                                    throw logic_error ("Unknown operator");
                                }
                            };
                            // pe = e_mt ();
                            pk = (*op)->ok;
                        }
                        break;
                    }
                    default: {
                        cout << "Unknown K tag: " << pk->tag << endl;
                        break;
                    }
                }
                break;
            }
            case TMPrm: {
                MPrm** prm = (MPrm**) &pc;
                switch ((*prm)->arity) {
                    case 0: {
                        switch ((*prm)->op) {
                            case TPRead: {
                                int i = 0;
                                scanf ("%d", &i);
                                MNum* n = (MNum*) malloc1 (sizeof(MNum));
                                n->m    = mk_m (TMNum);
                                n->val  = i;
                                pc = (M*) n;
                                break;
                            }
                            default: {
                                throw logic_error ("Unknown primitive op");
                            }
                        }
                        break;
                    }
                    case 1: {
                        M* nul = m_nul ();
                        KOp1* kop = (KOp1*) malloc1 (sizeof(KOp1));
                        M** ms = (M**) deref( (M*) (*prm)->ms );
                        kop->k  = mk_k (TKOp1);
                        kop->op = (*prm)->op;
                        kop->v  = nul;
                        kop->ok = pk;
                        pc = deref(ms[0]);
                        pk = (K*) kop;
                        break;
                    }
                    case 2: {
                        M* nul = m_nul ();
                        KOp2* kop = (KOp2*) malloc1 (sizeof(KOp2));
                        kop->k  = mk_k (TKOp2);
                        kop->op = (*prm)->op;
                        M** ms = (M**) deref((M*) (*prm)->ms);
                        kop->m  = deref(ms[1]);
                        kop->ok = pk;
                        kop->v  = nul;
                        pc  = deref(ms[0]);
                        pk  = (K*) kop;
                        break;
                    }
                    default: {
                        throw logic_error ("Unknown primitive operation arity");
                    }
                }
                break;
            }
            default: {
                cout << "Unknown M: " << tag << endl;
            }
        }
    }
}

int get_file_size (const char* filename) {
    struct stat buf;
    stat (filename, &buf);
    return buf.st_size;
}

void read_file (const char* filename) {

    // Map file into memory
    fd = open (filename, 0);
    fs = get_file_size (filename);
    mm = (char*) mmap (NULL, fs, PROT_READ, MAP_PRIVATE, fd, 0);

    // Read in length of byte code (pf)
    // and starting instruction (pc)
    int pos = *(int*)mm;

    pc = (M*) (mm + pos + 4);

    return;
}

void clean_up () {
    munmap (mm, fs);
    close (fd);
}

int main () {
    read_file ("tmp.byte");
    cek ();
    clean_up ();
    return 0;
}
