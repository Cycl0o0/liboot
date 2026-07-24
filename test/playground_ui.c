#include "playground_ui.h"

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FONT_FIRST 32
#define FONT_COUNT 96
#define CELL_W 8
#define CELL_H 8
#define ATLAS_COLS 16
#define ATLAS_ROWS 6
#define ATLAS_W ( ATLAS_COLS * CELL_W )
#define ATLAS_H ( ATLAS_ROWS * CELL_H )

static GLuint sFontTexture;
static int sUiActive;

/* Hand-authored 5x7 uppercase font. Lowercase is intentionally folded to
   uppercase: the playground is a diagnostics tool and this keeps the embedded
   font small, legible, and free of an SDL_ttf/runtime asset dependency. */
static const uint8_t sLetters[26][7] = {
    { 0x0E,0x11,0x11,0x1F,0x11,0x11,0x11 }, /* A */
    { 0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E }, /* B */
    { 0x0F,0x10,0x10,0x10,0x10,0x10,0x0F }, /* C */
    { 0x1E,0x11,0x11,0x11,0x11,0x11,0x1E }, /* D */
    { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F }, /* E */
    { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10 }, /* F */
    { 0x0F,0x10,0x10,0x13,0x11,0x11,0x0F }, /* G */
    { 0x11,0x11,0x11,0x1F,0x11,0x11,0x11 }, /* H */
    { 0x0E,0x04,0x04,0x04,0x04,0x04,0x0E }, /* I */
    { 0x07,0x02,0x02,0x02,0x12,0x12,0x0C }, /* J */
    { 0x11,0x12,0x14,0x18,0x14,0x12,0x11 }, /* K */
    { 0x10,0x10,0x10,0x10,0x10,0x10,0x1F }, /* L */
    { 0x11,0x1B,0x15,0x15,0x11,0x11,0x11 }, /* M */
    { 0x11,0x19,0x15,0x13,0x11,0x11,0x11 }, /* N */
    { 0x0E,0x11,0x11,0x11,0x11,0x11,0x0E }, /* O */
    { 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10 }, /* P */
    { 0x0E,0x11,0x11,0x11,0x15,0x12,0x0D }, /* Q */
    { 0x1E,0x11,0x11,0x1E,0x14,0x12,0x11 }, /* R */
    { 0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E }, /* S */
    { 0x1F,0x04,0x04,0x04,0x04,0x04,0x04 }, /* T */
    { 0x11,0x11,0x11,0x11,0x11,0x11,0x0E }, /* U */
    { 0x11,0x11,0x11,0x11,0x11,0x0A,0x04 }, /* V */
    { 0x11,0x11,0x11,0x15,0x15,0x15,0x0A }, /* W */
    { 0x11,0x11,0x0A,0x04,0x0A,0x11,0x11 }, /* X */
    { 0x11,0x11,0x0A,0x04,0x04,0x04,0x04 }, /* Y */
    { 0x1F,0x01,0x02,0x04,0x08,0x10,0x1F }, /* Z */
};

static const uint8_t sDigits[10][7] = {
    { 0x0E,0x11,0x13,0x15,0x19,0x11,0x0E },
    { 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E },
    { 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F },
    { 0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E },
    { 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02 },
    { 0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E },
    { 0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E },
    { 0x1F,0x01,0x02,0x04,0x08,0x08,0x08 },
    { 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E },
    { 0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E },
};

static void glyph_rows( unsigned char input, uint8_t rows[7] )
{
    memset( rows, 0, 7 );
    unsigned char c = (unsigned char)toupper( input );
    if( c >= 'A' && c <= 'Z' ) {
        memcpy( rows, sLetters[c - 'A'], 7 );
        return;
    }
    if( c >= '0' && c <= '9' ) {
        memcpy( rows, sDigits[c - '0'], 7 );
        return;
    }
    switch( c ) {
    case '!': { const uint8_t p[7] = {4,4,4,4,4,0,4}; memcpy(rows,p,7); } break;
    case '?': { const uint8_t p[7] = {14,17,1,2,4,0,4}; memcpy(rows,p,7); } break;
    case '.': rows[6] = 4; break;
    case ',': rows[5] = 4; rows[6] = 8; break;
    case ':': rows[2] = rows[5] = 4; break;
    case ';': rows[2] = 4; rows[5] = 4; rows[6] = 8; break;
    case '-': rows[3] = 14; break;
    case '_': rows[6] = 31; break;
    case '+': rows[1]=4; rows[2]=4; rows[3]=31; rows[4]=4; rows[5]=4; break;
    case '=': rows[2] = rows[4] = 31; break;
    case '/': rows[0]=1; rows[1]=2; rows[2]=2; rows[3]=4; rows[4]=8; rows[5]=8; rows[6]=16; break;
    case '\\': rows[0]=16; rows[1]=8; rows[2]=8; rows[3]=4; rows[4]=2; rows[5]=2; rows[6]=1; break;
    case '(': rows[0]=2; rows[1]=4; rows[2]=8; rows[3]=8; rows[4]=8; rows[5]=4; rows[6]=2; break;
    case ')': rows[0]=8; rows[1]=4; rows[2]=2; rows[3]=2; rows[4]=2; rows[5]=4; rows[6]=8; break;
    case '[': rows[0]=14; rows[1]=8; rows[2]=8; rows[3]=8; rows[4]=8; rows[5]=8; rows[6]=14; break;
    case ']': rows[0]=14; rows[1]=2; rows[2]=2; rows[3]=2; rows[4]=2; rows[5]=2; rows[6]=14; break;
    case '<': rows[1]=2; rows[2]=4; rows[3]=8; rows[4]=4; rows[5]=2; break;
    case '>': rows[1]=8; rows[2]=4; rows[3]=2; rows[4]=4; rows[5]=8; break;
    case '#': rows[1]=10; rows[2]=31; rows[3]=10; rows[4]=31; rows[5]=10; break;
    case '*': rows[1]=21; rows[2]=14; rows[3]=31; rows[4]=14; rows[5]=21; break;
    case '%': rows[0]=17; rows[1]=18; rows[2]=2; rows[3]=4; rows[4]=8; rows[5]=9; rows[6]=17; break;
    case '@': rows[0]=14; rows[1]=17; rows[2]=23; rows[3]=21; rows[4]=23; rows[5]=16; rows[6]=15; break;
    case '"': rows[0]=10; rows[1]=10; break;
    case '\'': rows[0]=4; rows[1]=4; break;
    case '|': rows[0]=4; rows[1]=4; rows[2]=4; rows[3]=4; rows[4]=4; rows[5]=4; rows[6]=4; break;
    case '^': rows[0]=4; rows[1]=10; rows[2]=17; break;
    case '~': rows[2]=9; rows[3]=22; break;
    case '$': rows[0]=4; rows[1]=15; rows[2]=20; rows[3]=14; rows[4]=5; rows[5]=30; rows[6]=4; break;
    case '&': rows[0]=12; rows[1]=18; rows[2]=20; rows[3]=8; rows[4]=21; rows[5]=18; rows[6]=13; break;
    default: break;
    }
}

int pg_ui_init( void )
{
    if( sFontTexture ) return 1;
    uint8_t pixels[ATLAS_W * ATLAS_H];
    memset( pixels, 0, sizeof( pixels ));
    for( int glyph = 0; glyph < FONT_COUNT; ++glyph ) {
        uint8_t rows[7];
        glyph_rows((unsigned char)( FONT_FIRST + glyph ), rows );
        int ox = ( glyph % ATLAS_COLS ) * CELL_W + 1;
        int oy = ( glyph / ATLAS_COLS ) * CELL_H;
        for( int y = 0; y < 7; ++y )
            for( int x = 0; x < 5; ++x )
                if( rows[y] & ( 1u << ( 4 - x )))
                    pixels[( oy + y ) * ATLAS_W + ox + x] = 255;
    }

    while( glGetError() != GL_NO_ERROR ) {}
    glGenTextures( 1, &sFontTexture );
    if( !sFontTexture ) return 0;
    glBindTexture( GL_TEXTURE_2D, sFontTexture );
    glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
    glTexImage2D( GL_TEXTURE_2D, 0, GL_ALPHA, ATLAS_W, ATLAS_H, 0,
                  GL_ALPHA, GL_UNSIGNED_BYTE, pixels );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
    if( glGetError() != GL_NO_ERROR ) {
        glDeleteTextures( 1, &sFontTexture );
        sFontTexture = 0;
        return 0;
    }
    return 1;
}

void pg_ui_shutdown( void )
{
    if( sFontTexture ) glDeleteTextures( 1, &sFontTexture );
    sFontTexture = 0;
    sUiActive = 0;
}

void pg_ui_begin( int drawableWidth, int drawableHeight )
{
    if( sUiActive || drawableWidth <= 0 || drawableHeight <= 0 ) return;
    sUiActive = 1;
    glPushAttrib( GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                  GL_TEXTURE_BIT | GL_CURRENT_BIT | GL_LINE_BIT );
    glDepthMask( GL_FALSE );
    glDisable( GL_DEPTH_TEST );
    glDisable( GL_ALPHA_TEST );
    glEnable( GL_BLEND );
    glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
    glMatrixMode( GL_PROJECTION );
    glPushMatrix();
    glLoadIdentity();
    glOrtho( 0, drawableWidth, drawableHeight, 0, -1, 1 );
    glMatrixMode( GL_MODELVIEW );
    glPushMatrix();
    glLoadIdentity();
}

void pg_ui_end( void )
{
    if( !sUiActive ) return;
    glMatrixMode( GL_MODELVIEW );
    glPopMatrix();
    glMatrixMode( GL_PROJECTION );
    glPopMatrix();
    glMatrixMode( GL_MODELVIEW );
    glDepthMask( GL_TRUE );
    glPopAttrib();
    sUiActive = 0;
}

void pg_ui_rect( float x, float y, float width, float height,
                 float r, float g, float b, float a )
{
    if( !sUiActive || width <= 0 || height <= 0 ) return;
    glDisable( GL_TEXTURE_2D );
    glColor4f( r, g, b, a );
    glBegin( GL_QUADS );
    glVertex2f( x, y );
    glVertex2f( x + width, y );
    glVertex2f( x + width, y + height );
    glVertex2f( x, y + height );
    glEnd();
}

void pg_ui_outline( float x, float y, float width, float height, float thickness,
                    float r, float g, float b, float a )
{
    if( thickness <= 0 ) return;
    pg_ui_rect( x, y, width, thickness, r, g, b, a );
    pg_ui_rect( x, y + height - thickness, width, thickness, r, g, b, a );
    pg_ui_rect( x, y + thickness, thickness, height - 2 * thickness, r, g, b, a );
    pg_ui_rect( x + width - thickness, y + thickness, thickness,
                height - 2 * thickness, r, g, b, a );
}

static void draw_text_run( float x, float y, float scale,
                           float r, float g, float b, float a, const char *text )
{
    if( !sFontTexture || !text || scale <= 0 ) return;
    const float startX = x;
    glEnable( GL_TEXTURE_2D );
    glBindTexture( GL_TEXTURE_2D, sFontTexture );
    glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
    glColor4f( r, g, b, a );
    glBegin( GL_QUADS );
    for( const unsigned char *p = (const unsigned char *)text; *p; ++p ) {
        if( *p == '\n' ) { x = startX; y += 9.0f * scale; continue; }
        unsigned char c = *p;
        if( c < FONT_FIRST || c >= FONT_FIRST + FONT_COUNT ) c = '?';
        int glyph = c - FONT_FIRST;
        int col = glyph % ATLAS_COLS, row = glyph / ATLAS_COLS;
        float u0 = (float)( col * CELL_W ) / ATLAS_W;
        float v0 = (float)( row * CELL_H ) / ATLAS_H;
        float u1 = (float)( col * CELL_W + CELL_W ) / ATLAS_W;
        float v1 = (float)( row * CELL_H + CELL_H ) / ATLAS_H;
        float w = CELL_W * scale, h = CELL_H * scale;
        glTexCoord2f( u0, v0 ); glVertex2f( x, y );
        glTexCoord2f( u1, v0 ); glVertex2f( x + w, y );
        glTexCoord2f( u1, v1 ); glVertex2f( x + w, y + h );
        glTexCoord2f( u0, v1 ); glVertex2f( x, y + h );
        x += 6.0f * scale;
    }
    glEnd();
    glDisable( GL_TEXTURE_2D );
}

void pg_ui_text( float x, float y, float scale,
                 float r, float g, float b, float a, const char *text )
{
    if( !sUiActive || !text ) return;
    draw_text_run( x + scale, y + scale, scale, 0, 0, 0, a * 0.75f, text );
    draw_text_run( x, y, scale, r, g, b, a, text );
}

void pg_ui_textf( float x, float y, float scale,
                  float r, float g, float b, float a, const char *format, ... )
{
    if( !format ) return;
    char buffer[512];
    va_list args;
    va_start( args, format );
    vsnprintf( buffer, sizeof( buffer ), format, args );
    va_end( args );
    buffer[sizeof( buffer ) - 1] = '\0';
    pg_ui_text( x, y, scale, r, g, b, a, buffer );
}

float pg_ui_text_width( const char *text, float scale )
{
    if( !text || scale <= 0 ) return 0;
    size_t current = 0, longest = 0;
    for( ; *text; ++text ) {
        if( *text == '\n' ) { if( current > longest ) longest = current; current = 0; }
        else current++;
    }
    if( current > longest ) longest = current;
    return longest ? ( (float)longest * 6.0f + 2.0f ) * scale : 0;
}

float pg_ui_line_height( float scale )
{
    return 9.0f * scale;
}
