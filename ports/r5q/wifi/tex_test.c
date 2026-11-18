/*
 * tex_test: what the GPU (Mesa freedreno through libOSMesa) returns when sampling a texture of each of the formats
 * CoreAnimation fills (luminance+alpha, alpha, luminance, RGBA), against what was put in. (r5q, Test 166: rounded
 * corners come out as octagons on the Adreno 640; a corner mask that samples wrong would do that.)
 *   tex_test            four texels a format, each drawn as a quarter of an 8 x 2 picture and read back
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct osmesa_context *OSMesaContext;
extern OSMesaContext OSMesaCreateContextAttribs(const int *attributes, OSMesaContext share);
extern unsigned char OSMesaMakeCurrent(OSMesaContext context, void *buffer, unsigned type, int width, int height);
extern const unsigned char *glGetString(unsigned name);
extern unsigned glGetError(void);
extern void glGenTextures(int n, unsigned *names);
extern void glBindTexture(unsigned target, unsigned texture);
extern void glTexImage2D(unsigned target, int level, int internal, int width, int height, int border, unsigned format,
    unsigned type, const void *pixels);
extern void glTexParameteri(unsigned target, unsigned name, int value);
extern void glPixelStorei(unsigned name, int value);
extern void glViewport(int x, int y, int width, int height);
extern void glClearColor(float red, float green, float blue, float alpha);
extern void glClear(unsigned mask);
extern unsigned glCreateShader(unsigned type);
extern void glShaderSource(unsigned shader, int count, const char *const *strings, const int *lengths);
extern void glCompileShader(unsigned shader);
extern unsigned glCreateProgram(void);
extern void glAttachShader(unsigned program, unsigned shader);
extern void glBindAttribLocation(unsigned program, unsigned index, const char *name);
extern void glLinkProgram(unsigned program);
extern void glUseProgram(unsigned program);
extern void glVertexAttribPointer(unsigned index, int size, unsigned type, unsigned char normalized, int stride,
    const void *pointer);
extern void glEnableVertexAttribArray(unsigned index);
extern void glDrawArrays(unsigned mode, int first, int count);
extern void glFinish(void);
extern void glReadPixels(int x, int y, int width, int height, unsigned format, unsigned type, void *pixels);

static unsigned
shader(unsigned type, const char *text)
{
	unsigned name = glCreateShader(type);

	glShaderSource(name, 1, &text, NULL);
	glCompileShader(name);
	return name;
}

int
main(void)
{
	static const int attributes[] = { 0x22, 0x1908, 0x33, 0x38, 0x36, 2, 0 };
	static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	static const struct {
		const char *name;
		unsigned format, bytes;
	} kinds[] = { { "luminance+alpha", 0x190a, 2 }, { "alpha", 0x1906, 1 }, { "luminance", 0x1909, 1 }, { "rgba", 0x1908, 4 } };
	static uint8_t window[8 * 2 * 4];
	uint8_t texels[16], back[8 * 2 * 4];
	unsigned program, texture;
	OSMesaContext context = OSMesaCreateContextAttribs(attributes, NULL);

	if (context == NULL || !OSMesaMakeCurrent(context, window, 0x1401, 8, 2)) {
		printf("tex_test: no context\n");
		return 1;
	}
	printf("tex_test: %s\n", (const char *)glGetString(0x1f01));
	program = glCreateProgram();
	glAttachShader(program, shader(0x8b31, "attribute vec2 where; varying vec2 at;\n"
	    "void main() { gl_Position = vec4(where, 0.0, 1.0); at = where * 0.5 + 0.5; }\n"));
	glAttachShader(program, shader(0x8b30, "precision mediump float; varying vec2 at; uniform sampler2D t;\n"
	    "void main() { gl_FragColor = texture2D(t, at); }\n"));
	glBindAttribLocation(program, 0, "where");
	glLinkProgram(program);
	glUseProgram(program);
	glVertexAttribPointer(0, 2, 0x1406, 0, 0, quad);
	glEnableVertexAttribArray(0);
	glPixelStorei(0x0cf5, 1);               /* GL_UNPACK_ALIGNMENT */
	glViewport(0, 0, 8, 2);
	for (unsigned k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
		for (unsigned i = 0; i < sizeof(texels); i++) {
			texels[i] = (uint8_t)(17 + 40 * i);     /* 17, 57, 97, ... (mod 256) */
		}
		glGenTextures(1, &texture);
		glBindTexture(0x0de1, texture);
		glTexParameteri(0x0de1, 0x2801, 0x2600);        /* nearest */
		glTexParameteri(0x0de1, 0x2800, 0x2600);
		glTexImage2D(0x0de1, 0, (int)kinds[k].format, 4, 1, 0, kinds[k].format, 0x1401, texels);
		glClearColor(0.5f, 0.5f, 0.5f, 0.5f);
		glClear(0x4000);
		glDrawArrays(5, 0, 4);
		glFinish();
		memset(back, 0, sizeof(back));
		glReadPixels(0, 0, 8, 2, 0x1908, 0x1401, back);
		printf("tex_test: %-16s put", kinds[k].name);
		for (unsigned t = 0; t < 4; t++) {
			printf(" [");
			for (unsigned b = 0; b < kinds[k].bytes; b++) {
				printf("%s%u", b ? " " : "", texels[t * kinds[k].bytes + b]);
			}
			printf("]");
		}
		printf("; got (rgba)");
		for (unsigned t = 0; t < 4; t++) {
			const uint8_t *p = back + (t * 2 + 1) * 4;     /* texel t covers pixels 2t, 2t+1 of each row */
			printf(" [%u %u %u %u]", p[0], p[1], p[2], p[3]);
		}
		printf("; error %#x\n", glGetError());
	}
	return 0;
}
