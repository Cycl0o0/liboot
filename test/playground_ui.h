#ifndef LIBOOT_PLAYGROUND_UI_H
#define LIBOOT_PLAYGROUND_UI_H

#include <stdarg.h>

/* Tiny dependency-free OpenGL 2.1 UI renderer used only by the playground.
   Coordinates are drawable pixels with a top-left origin. */
int pg_ui_init( void );
void pg_ui_shutdown( void );
void pg_ui_begin( int drawableWidth, int drawableHeight );
void pg_ui_end( void );

void pg_ui_rect( float x, float y, float width, float height,
                 float r, float g, float b, float a );
void pg_ui_outline( float x, float y, float width, float height, float thickness,
                    float r, float g, float b, float a );
void pg_ui_text( float x, float y, float scale,
                 float r, float g, float b, float a, const char *text );
void pg_ui_textf( float x, float y, float scale,
                  float r, float g, float b, float a, const char *format, ... );
float pg_ui_text_width( const char *text, float scale );
float pg_ui_line_height( float scale );

#endif
