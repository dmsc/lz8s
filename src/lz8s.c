/*
 * LZ8S ultra-simple LZ based compressor
 * -------------------------------------
 *
 * (c) 2025 DMSC
 * Code under MIT license, see LICENSE file.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
void set_binary(void)
{
  setmode(fileno(stdout),O_BINARY);
  setmode(fileno(stdin),O_BINARY);
}
#else
void set_binary(void)
{
}
#endif

// Big number, used to signal invalid matches
#define INFINITE_COST   (INT_MAX/256)

// Statistics
static int *stat_llen;
static int *stat_mlen;
static int *stat_moff;

///////////////////////////////////////////////////////
// Bit encoding functions
struct bf
{
    int len;
    uint8_t buf[65536];
    int total;
    FILE *out;
};

static void init(struct bf *x)
{
    x->total = 0;
    x->len = 0;
}

static void bflush(struct bf *x)
{
    if( x->len )
        fwrite(x->buf, x->len, 1, x->out);
    x->total += x->len;
    x->len = 0;
}


static void add_byte(struct bf *x, int byte)
{
    x->buf[x->len] = byte;
    x->len ++;
}

///////////////////////////////////////////////////////
// LZ8S compression functions
static int max(int a, int b)
{
    return a>b ? a : b;
}

static int get_mlen(const uint8_t *a, const uint8_t *b, int max)
{
    for(int i=0; i<max; i++)
        if( a[i] != b[i] )
            return i;
    return max;
}

int hsh(const uint8_t *p)
{
    size_t x = (size_t)p;
    return 0xFF & (x ^ (x>>8) ^ (x>>16) ^ (x>>24));
}

static int bits_moff = 8;       // Number of bits used for OFFSET
static int min_mlen = 1;        // Minimum match length
static int max_mlen = 255;      // Maximum match length (unlimited in LZ4)
static int max_llen = 255;      // Maximum literal length (unlimited in LZ4)
static int zero_offset = 0;     // Do not write offset on matches of length 0
static int zero_match_cost = 0; // Cost of a zero-length match


#define max_off (1<<bits_moff)  // Maximum offset

// Struct for LZ optimal parsing
struct lzop_st {
    int lbits;      // Number of bits needed to code LITERAL from position
    int llen;       // Literal length at position
    int mbits;      // Number of bits needed to code MATCH from position
    int mlen;       // Match length at position
    int mpos;       // Best match offset at position
};

struct lzop
{
    const uint8_t *data;// The data to compress
    int size;           // Data size
    struct lzop_st *sp; // State at each position
    int in_literal;     // Inside match during encoding
    int bytes_literal;  // Bytes encoded as literal
    int bytes_matches;  // Bytes encoded as matches
    int bits_literal;   // Bits used to encode literal lengths
    int bits_matches;   // Bits used to encode matches
};

// Returns maximal match length (and match position) at pos.
static int match(const uint8_t *data, int pos, int size, int *mpos)
{
    int mxlen = -max(-max_mlen, pos - size);
    int mlen = 0;
    for(int i=max(pos-max_off,0); i<pos; i++)
    {
        int ml = get_mlen(data + pos, data + i, mxlen);
        if( ml > mlen )
        {
            mlen = ml;
            *mpos = pos - i;
            if( mlen >= mxlen )
                return mlen;
        }
    }
    return mlen;
}

// Returns the cost of writing this length
static int mlen_cost(int l)
{
    if( l > max_mlen )
        return INFINITE_COST; // Infinite cost
    else if( max_mlen > 255 && l > 127 )
        return 16; // Two byte length
    else
        return 8;
}

// Returns the cost of writing the match offset
static int moff_cost(int o)
{
    if( o > max_off || o < 1 )
        return INFINITE_COST;
    if (!bits_moff)
        return 0;
    else if(bits_moff <= 8)
        return 8;
    else
        return 16;
}

// Returns the cost of writing this length
static int llen_cost(int l)
{
    int bits = 0;
    if( !l )
        return 0;
    while( l > max_llen ) {
        // Encode a match of zero length plus the max length
        bits += 8 + zero_match_cost;
        l -= max_llen;
    }
    // Two byte length
    if( max_llen > 255 && l > 127 )
        bits += 8;
    return 8 + bits;
}

static void lzop_init(struct lzop *lz, const uint8_t *data, int size)
{
    lz->data  = data;
    lz->size  = size;
    lz->sp    = calloc(sizeof(lz->sp[0]), size + 1);
    lz->in_literal = 0;
    lz->bytes_literal = 0;
    lz->bytes_matches = 0;
    lz->bits_literal = 0;
    lz->bits_matches = 0;
    zero_match_cost = mlen_cost(0) + (zero_offset ? moff_cost(1) : 0);
}

static void lzop_backfill(struct lzop *lz)
{
    if(!lz->size)
        return;

    // Initialize the last byte
    {
        struct lzop_st *cur = &(lz->sp[lz->size]);

        // At end, we can assume is a literal of size 0
        cur->llen  = 0;
        cur->lbits = 0;
        cur->mlen  = 0;
        cur->mpos  = 0;
        cur->mbits = INFINITE_COST;
    }

    // Go backwards in file storing best parsing
    for(int pos = lz->size - 1; pos>=0; pos--)
    {
        // Get best match at this position
        int mp = 0;
        struct lzop_st *cur = &(lz->sp[pos]);

        // Init literal case, best of two cases:
        int ml = 0;
        cur->lbits = INFINITE_COST;
        cur->llen = 0;
        //  LITERAL after this literal,
        //  encode 1 byte of literal plus updating the literal length.
        for(int i = 1; i < 6; i ++)
        {
            if (pos + i > lz->size)
                break;
            struct lzop_st *nxt = &(lz->sp[pos+i]);
            if( ml < nxt->llen + i )
                ml = nxt->llen + i;
            int lbits = nxt->lbits + 8 * i - llen_cost(nxt->llen) + llen_cost(nxt->llen + i);
            if( lbits < cur->lbits )
            {
                cur->lbits = lbits;
                cur->llen = nxt->llen + i;
            }
        }

        //  MATCH after N bytes of literal,
        //  encode 1 byte of literal plus the full match, search up to max
        //  "available" literal length
        for(int i = 1; i <= ml - 1; i++)
        {
            struct lzop_st *nxt = &(lz->sp[pos+i]);
            int mbits = nxt->mbits + 8 * i + llen_cost(i);
            if( mbits < cur->lbits )
            {
                cur->llen = i;
                cur->lbits = mbits;
            }
        }

        // Check all posible match lengths, store best
        ml = match(lz->data , pos, lz->size, &mp);
        int bestm = INFINITE_COST;
        cur->mbits = INFINITE_COST;
        cur->mpos = mp;
        for(int l=min_mlen; l <= ml; l++)
        {
            struct lzop_st *nxt = &(lz->sp[pos + l]);

            // MATCH after:
            //   If we land in another MATCH, we need to encode a new literal
            //   of length 0 there, so adds a byte
            int mbits = nxt->mbits + llen_cost(1) + moff_cost(mp) + mlen_cost(l);
            // LITERAL after
            int lbits = nxt->lbits + moff_cost(mp) + mlen_cost(l);

            // TODO: how to resolve ties mbits/lbits??
            // The order of te comparisons bellow, or using < instead of <= does
            // not seem to affect compression.
            if( lbits <= bestm )
            {
                bestm = lbits;
                cur->mlen = l;
                cur->mbits = bestm;
            }
            if( mbits <= bestm )
            {
                bestm = mbits;
                cur->mlen = l;
                cur->mbits = bestm;
            }
        }
    }
}

static void debug_encode(struct lzop *lz, int sz)
{
    int in_literal = 0;
    int pos = 0;
#if 0
    for(int i = 0; i < sz; i++)
    {
        struct lzop_st *cur = &(lz->sp[i]);
        int cm = cur->mbits >= INFINITE_COST ? -1 : cur->mbits;
        fprintf(stderr, "[%04X]: (%6d:%6d) (%d:%d) (%04X:%04X)\n", i, cur->lbits, cm,
                cur->llen, cur->mlen, i + cur->llen, i + cur->mlen);
    }
#endif
    while(pos < sz)
    {
        struct lzop_st *cur = &(lz->sp[pos]);
        int cm = cur->mbits >= INFINITE_COST ? -1 : cur->mbits;
        fprintf(stderr, "[%04X]: (%6d:%6d) ", pos, cur->lbits, cm);
        int extra_cost = in_literal ? zero_match_cost : 0;
        if( cur->lbits + extra_cost <= cur->mbits )
        {
            int len = cur->llen;
            int cost = llen_cost(len) + len * 8;
            if(in_literal)
            {
                fprintf(stderr, "M0 (%4d)\n                        ",
                        zero_match_cost/8 );
                cost = cost + zero_match_cost;
            }
            fprintf(stderr, "L %3d %4d | %6d -%5d ->%6d\n",
                    len, llen_cost(len) / 8 + len,
                    cur->lbits, cost, cur->lbits - cost);
            pos += len;
            in_literal = 1;
        }
        else
        {
            int mpos = cur->mpos;
            int len = cur->mlen;
            int cost = mlen_cost(len) + moff_cost(mpos);
            if(!in_literal)
            {
                fprintf(stderr, "L0 (%4d)\n                        ", llen_cost(0));
                cost = cost + llen_cost(0);
            }
            fprintf(stderr, "M %3d %4d | %6d -%5d ->%6d\n",
                    len, (mlen_cost(len) + moff_cost(mpos))/8,
                    cur->mbits, cost, cur->mbits - cost);
            pos += len;
            in_literal = 0;
        }
    }
}

static void code_match(struct bf *b, struct lzop *lz, int len, int off)
{
    // Keep statistics as a match if len > 0, literal otherwise
    int *bits = len ? &lz->bits_matches : &lz->bits_literal;

    if(len > 127 && max_mlen > 255)
    {
        add_byte(b, (0x80 | len) & 0xFF);
        add_byte(b, (len >> 7) - 1);
        *bits += 16;
    }
    else
    {
        add_byte(b, len & 0xFF);
        *bits += 8;
    }
    if(len || zero_offset)
    {
        if( bits_moff )
        {
            add_byte(b, off & 0xFF );
            *bits += 8;
        }
        if( bits_moff > 8 )
        {
            add_byte(b, off >> 8 );
            *bits += 8;
        }
    }
}

static int lzop_encode(struct bf *b, struct lzop *lz, int pos, int lpos, int offset_rel)
{
    if( pos <= lpos )
    {
        // We are skipping, output byte if the skip is a literal
        if( lz->in_literal )
        {
            add_byte(b, lz->data[pos]);
            lz->bytes_literal ++;
        }
        else
            lz->bytes_matches ++;
        return lpos;
    }

    // Encode best from filled table
    struct lzop_st *cur = &(lz->sp[pos]);
    int extra_cost = lz->in_literal ? zero_match_cost : 0;
    if( cur->lbits + extra_cost <= cur->mbits )
    {
        // Literal just encode the byte
        int len = cur->llen;
        if( len > max_llen )
            len = max_llen;
        // Already on literal - encode a zero length match to terminate
        if( lz->in_literal )
            code_match(b, lz, 0, 0);
        // Encode new literal count
        if( max_llen > 255 && len > 127 )
        {
            add_byte(b, (0x80 | len) & 0xFF);
            add_byte(b, (len >> 7) - 1);
            lz->bits_literal += 16;
        }
        else
        {
            add_byte(b, len & 0xFF);
            lz->bits_literal += 8;
        }
        stat_llen[len]++;
        // And first literal
        add_byte(b, lz->data[pos]);
        lz->bytes_literal ++;
        lz->in_literal = 1;
        return pos + len - 1;
    }
    else
    {
        int mpos = cur->mpos;
        int mlen = cur->mlen;
        stat_mlen[mlen]++;
        stat_moff[mpos]++;
        if( offset_rel < 0 )
            mpos = (mpos - 1) & 0xFFFF;
        else
            mpos = (pos + offset_rel - mpos) & 0xFFFF;
        if( !lz->in_literal )
        {
            // Already on match - encode a zero length literal
            add_byte(b, 0);
            stat_llen[0]++;
            lz->bits_matches += 8;
        }
        code_match(b, lz, mlen, mpos);
        lz->bytes_matches ++;
        lz->in_literal = 0;
        return pos + mlen - 1;
    }
}

static const char *prog_name;
static void cmd_error(const char *msg)
{
    fprintf(stderr,"%s: error, %s\n"
            "Try '%s -h' for help.\n", prog_name, msg, prog_name);
    exit(1);
}

///////////////////////////////////////////////////////
int main(int argc, char **argv)
{
    struct bf b;
    uint8_t *data;
    int lpos;
    int show_stats = 1;
    int offset_rel = -1;
    int print_debug = 0;

    prog_name = argv[0];
    int opt;
    while( -1 != (opt = getopt(argc, argv, "hqvndo:l:m:A:")) )
    {
        switch(opt)
        {
            case 'o':
                bits_moff = atoi(optarg);
                break;
            case 'l':
                max_llen = atoi(optarg);
                break;
            case 'm':
                max_mlen = atoi(optarg);
                break;
            case 'A':
                offset_rel = strtol(optarg, 0, 0);
                break;
            case 'd':
                print_debug = 1;
                break;
            case 'n':
                zero_offset = 1;
                break;
            case 'v':
                show_stats = 2;
                break;
            case 'q':
                show_stats = 0;
                break;
            case 'h':
            default:
                fprintf(stderr,
                       "LZ8S-X ultra-simple LZ based compressor - by dmsc.\n"
                       "\n"
                       "Usage: %s [options] <input_file> <output_file>\n"
                       "\n"
                       "If output_file is omitted, write to standard output, and if\n"
                       "input_file is also omitted, read from standard input.\n"
                       "\n"
                       "Options:\n"
                       "  -o BITS  Sets match offset bits (default = %d).\n"
                       "  -l NUM   Sets max literal run length (default = %d).\n"
                       "  -m NUM   Sets max match run length (default = %d).\n"
                       "  -A ADDR  Encode position relative to address instead of offset.\n"
                       "  -n       Do not omit match offset on zero match length.\n"
                       "  -v       Shows match length/offset statistics.\n"
                       "  -d       Shows debug information on compression chain.\n"
                       "  -q       Don't show detailed compression stats.\n"
                       "  -h       Shows this help.\n",
                       prog_name, bits_moff, max_llen, max_mlen);
                exit(EXIT_FAILURE);
        }
    }

    // Check option values
    if( max_mlen < 1 || max_mlen > 32895 )
        cmd_error("max match run length should be from 1 to 32895");
    if( max_llen < 1 || max_llen > 32895 )
        cmd_error("max literal run length should be from 1 to 32895");
    if( bits_moff < 0 || bits_moff > 16 )
        cmd_error("match offset bits should be from 0 to 16");
    if(bits_moff == 8)
    {
        if(offset_rel > 0xFF)
            cmd_error("relative address should be less than 256 with 8 bit offsets");
    }
    else if(bits_moff == 16)
    {
        if( offset_rel > 0xFFFF )
            cmd_error("relative address should be less than 65536");
    }
    else if(offset_rel >= 0)
        cmd_error("relative address works only with 8 or 16 bit offsets");

    if( optind < argc-2 )
        cmd_error("too many arguments: one input file and one output file expected");

    FILE *input_file = stdin;
    if( optind < argc )
    {
        input_file = fopen(argv[optind], "rb");
        if( !input_file )
        {
            fprintf(stderr, "%s: can't open input file '%s': %s\n",
                    prog_name, argv[optind], strerror(errno));
            exit(EXIT_FAILURE);
        }
    }
    // Set stdin and stdout as binary files
    set_binary();

    // Alloc statistic arrays
    stat_llen = calloc(sizeof(int), max_llen + 1);
    stat_mlen = calloc(sizeof(int), max_mlen + 1);
    stat_moff = calloc(sizeof(int), max_off + 1);

    // Max size of bufer: 128k
    data = malloc(128*1024); // calloc(128,1024);
    lpos = -1;

    // Read all data
    int sz = fread(data, 1, 128*1024, input_file);

    // Close file
    if( input_file != stdin )
        fclose(input_file);

    // Open output file if needed
    FILE *output_file = stdout;
    if( optind < argc-1 )
    {
        output_file = fopen(argv[optind+1], "wb");
        if( !output_file )
        {
            fprintf(stderr, "%s: can't open output file '%s': %s\n",
                    prog_name, argv[optind+1], strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    b.out = output_file;
    init(&b);

    // Init LZ state
    struct lzop lz;
    lzop_init(&lz, data, sz);
    lzop_backfill(&lz);

    // Write encode walk:
    if(print_debug)
        debug_encode(&lz, sz);

    // Compress
    init(&b);
    for(int pos = 0; pos < sz; pos++)
        lpos = lzop_encode(&b, &lz, pos, lpos, offset_rel);

    bflush(&b);
    // Close file
    if( output_file != stdout )
        fclose(input_file);
    else
        fflush(stdout);


    // Show stats
    fprintf(stderr,"LZ8S: max offset= %d,\tmax mlen= %d,\tmax llen= %d,\t",
            max_off, max_mlen, max_llen);
    fprintf(stderr,"ratio: %5d / %d = %5.2f%%\n", b.total, sz, (100.0*b.total) / (sz));
    if( show_stats )
    {
        double total1 = 100.0 / sz;
        double total2 = 100.0 / b.total;
        int bits = lz.sp[0].mbits < lz.sp[0].lbits ? lz.sp[0].mbits : lz.sp[0].lbits;
        fprintf(stderr, " Total size estimated %d bits", bits);
        if( b.total * 8 - bits )
            fprintf(stderr, "(difference of %d with real)", b.total * 8 - bits);
        fprintf(stderr, "\n"
                        " Compression Information:                Input  Output\n"
                        " Bytes encoded as matches: %5d bytes,  %4.1f%%     -\n"
                        " Bytes encoded as literal: %5d bytes,  %4.1f%%   %4.1f%%\n"
                        " Total matches overhead: %7d bits,     -     %4.1f%%\n"
                        " Total literal overhead: %7d bits,     -     %4.1f%%\n",
                lz.bytes_matches, total1 * lz.bytes_matches,
                lz.bytes_literal, total1 * lz.bytes_literal, total2 * lz.bytes_literal,
                lz.bits_matches, total2 * 0.125 * lz.bits_matches,
                lz.bits_literal, total2 * 0.125 * lz.bits_literal);

        if( show_stats > 1 )
        {
            fprintf(stderr,"\nvalue\t  MPOS\t  MLEN\t  LLEN\n");
            for(int i=0; i<=max_mlen || i<=max_off || i<=max_llen; i++)
            {
                fprintf(stderr,"%2d\t%5d\t%5d\t%5d\n", i,
                        (i <= max_off) ? stat_moff[i] : 0,
                        (i <= max_mlen) ? stat_mlen[i] : 0,
                        (i <= max_llen) ? stat_llen[i] : 0);
            }
        }
    }

    free(stat_llen);
    free(stat_mlen);
    free(stat_moff);
    return 0;
}

