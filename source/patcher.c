#include <3ds.h>
#include <string.h>
#include "patcher.h"
#include "ifile.h"

#ifndef PATH_MAX
#define PATH_MAX 255
#endif

// Below is stolen from http://en.wikipedia.org/wiki/Boyer%E2%80%93Moore_string_search_algorithm

#define ALPHABET_LEN 256
#define NOT_FOUND patlen
#define max(a, b) ((a < b) ? b : a)
 
// delta1 table: delta1[c] contains the distance between the last
// character of pat and the rightmost occurence of c in pat.
// If c does not occur in pat, then delta1[c] = patlen.
// If c is at string[i] and c != pat[patlen-1], we can
// safely shift i over by delta1[c], which is the minimum distance
// needed to shift pat forward to get string[i] lined up 
// with some character in pat.
// this algorithm runs in alphabet_len+patlen time.
static void make_delta1(int *delta1, u8 *pat, int patlen){
    int i;
    for (i=0; i < ALPHABET_LEN; i++) delta1[i] = NOT_FOUND;
    for (i=0; i < patlen-1; i++) delta1[pat[i]] = patlen-1 - i;
}
 
// true if the suffix of word starting from word[pos] is a prefix 
// of word
static int is_prefix(u8 *word, int wordlen, int pos){
    int i, suffixlen = wordlen - pos;
    for (i = 0; i < suffixlen; i++) {
        if (word[i] != word[pos+i]) return 0;
    }
    return 1;
}
 
// length of the longest suffix of word ending on word[pos].
// suffix_length("dddbcabc", 8, 4) = 2
static int suffix_length(u8 *word, int wordlen, int pos){
    int i;
    // increment suffix length i to the first mismatch or beginning
    // of the word
    for (i = 0; (word[pos-i] == word[wordlen-1-i]) && (i < pos); i++);
    return i;
}
 
// delta2 table: given a mismatch at pat[pos], we want to align 
// with the next possible full match could be based on what we
// know about pat[pos+1] to pat[patlen-1].
//
// In case 1:
// pat[pos+1] to pat[patlen-1] does not occur elsewhere in pat,
// the next plausible match starts at or after the mismatch.
// If, within the substring pat[pos+1 .. patlen-1], lies a prefix
// of pat, the next plausible match is here (if there are multiple
// prefixes in the substring, pick the longest). Otherwise, the
// next plausible match starts past the character aligned with 
// pat[patlen-1].
// 
// In case 2:
// pat[pos+1] to pat[patlen-1] does occur elsewhere in pat. The
// mismatch tells us that we are not looking at the end of a match.
// We may, however, be looking at the middle of a match.
// 
// The first loop, which takes care of case 1, is analogous to
// the KMP table, adapted for a 'backwards' scan order with the
// additional restriction that the substrings it considers as 
// potential prefixes are all suffixes. In the worst case scenario
// pat consists of the same letter repeated, so every suffix is
// a prefix. This loop alone is not sufficient, however:
// Suppose that pat is "ABYXCDEYX", and text is ".....ABYXCDEYX".
// We will match X, Y, and find B != E. There is no prefix of pat
// in the suffix "YX", so the first loop tells us to skip forward
// by 9 characters.
// Although superficially similar to the KMP table, the KMP table
// relies on information about the beginning of the partial match
// that the BM algorithm does not have.
//
// The second loop addresses case 2. Since suffix_length may not be
// unique, we want to take the minimum value, which will tell us
// how far away the closest potential match is.
static void make_delta2(int *delta2, u8 *pat, int patlen){
    int p;
    int last_prefix_index = patlen-1;
  
    // first loop
    for (p=patlen-1; p>=0; p--) {
        if (is_prefix(pat, patlen, p+1)) {
            last_prefix_index = p+1;
        }
        delta2[p] = last_prefix_index + (patlen-1 - p);
    }
 
    // second loop
    for (p=0; p < patlen-1; p++) {
        int slen = suffix_length(pat, patlen, p);
        if (pat[p - slen] != pat[patlen-1 - slen]) {
            delta2[patlen-1 - slen] = patlen-1 - p + slen;
        }
    }
}
 
static u8* boyer_moore(u8 *string, int stringlen, u8 *pat, int patlen){
    int i;
    int delta1[ALPHABET_LEN];
    int delta2[patlen * sizeof(int)];
    make_delta1(delta1, pat, patlen);
    make_delta2(delta2, pat, patlen);
 
  i = patlen-1;
    while (i < stringlen) {
        int j = patlen-1;
        while (j >= 0 && (string[i] == pat[j])) {
            --i;
            --j;
        }
        if (j < 0) return (string + i+1);
        i += max(delta1[string[i]], delta2[j]);
    }
    return NULL;
}

static int patch_memory(start, size, pattern, patsize, offset, replace, repsize, count)
    u8* start;
    u32 size;
    u32 patsize;
    u8 pattern[patsize];
    int offset;
    u32 repsize;
    u8 replace[repsize];
    int count;
{
    u8 *found;
    int i;
    u32 at;

    for (i = 0; i < count; i++){
        found = boyer_moore(start, size, pattern, patsize);
        if (found == NULL) break;
        at = (u32)(found - start);
        memcpy(found + offset, replace, repsize);
        if (at + patsize > size) size = 0;
        else size = size - (at + patsize);
        start = found + patsize;
    }
    return i;
}

int patch_code(u64 progid, u8 *code, u32 size){
    //File vars
    IFile fp;
    FS_Path apath;
    FS_Path ppath;
    size_t len;
    u64 fileSize;
    const char *path = "/rei/patches/patches.dat";
    
    //Patch vars
    u64 read_id, 
        br;
    u8 pattern_length, 
       patch_length;
    s8 first_offset, 
       second_offset;
    u8 pattern[0x100];
    u8 patch[0x100];
    
    len = strnlen(path, PATH_MAX);
    apath.type = PATH_EMPTY;
    apath.size = 1;
    apath.data = (u8 *)"";
    ppath.type = PATH_ASCII;
    ppath.data = path;
    ppath.size = len+1;
    
    //Read patches from SD
    if(R_FAILED(IFile_Open(&fp, ARCHIVE_SDMC, apath, ppath, FS_OPEN_READ))) goto end;
    if(R_FAILED(IFile_GetSize(&fp, &fileSize))) goto end;
    while (!feof(&fp)) { //Read all patches concatinated
        if (R_FAILED(IFile_Read(&fp, &br, &read_id, 8))) goto end;
        if (R_FAILED(IFile_Read(&fp, &br, &pattern_length, 1))) goto end;
        if (R_FAILED(IFile_Read(&fp, &br, &patch_length, 1))) goto end;
        if (R_FAILED(IFile_Read(&fp, &br, &first_offset, 1))) goto end;
        if (R_FAILED(IFile_Read(&fp, &br, &second_offset, 1))) goto end;
        if (R_FAILED(IFile_Read(&fp, &br, &pattern, pattern_length))) goto end;
        if (R_FAILED(IFile_Read(&fp, &br, &patch, patch_length))) goto end;
        if (read_id == progid) patch_memory(code, size, pattern, pattern_length, first_offset, patch, patch_length, second_offset);
    }
    end:
    IFile_Close(&fp);
    
    //Hardcoding Rei string here so it cant be changed ;^)
    switch(progid){
        case 0x0004001000020000LL:  //JPN MSET
        case 0x0004001000021000LL:  //USA MSET
        case 0x0004001000022000LL:  //EUR MSET
        case 0x0004001000026000LL:  //CHN MSET
        case 0x0004001000027000LL:  //KOR MSET
        case 0x0004001000028000LL:  //TWN MSET
        {
            static const char* ver_string_pattern = u"Ver.";
            static const char* ver_string_patch = u"\uE024Rei";
            patch_memory(code, size, 
            ver_string_pattern, 8, 0, 
            ver_string_patch, 8, 1
            );
            break;
        }
    }
    return 0;
}