#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <float.h> // FLT_MAX
#include <glad/glad.h>
//#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "utils.h"
#include "camera.h"
#include "vector.h"
#include "thread.h"
#include "sync.h"
#include "mesh.h"

typedef struct {
	Vector3 albedo;
	float   roughness;
	float   reflectance;
	float   metallic;
	float   emission_power;
	Vector3 emission_color;
} Material;

#ifndef M_PI
#define M_PI 3.1415926538
#endif

int screen_w;
int screen_h;
os_mutex_t screen_mutex;

float maxf(float x, float y) { return x > y ? x : y; }
float minf(float x, float y) { return x < y ? x : y; }
float absf(float x) { return x < 0 ? -x : x; }

float clamp(float x, float min, float max)
{
	assert(min <= max);
	if (x < min) return min;
	if (x > max) return max;
	return x;
}

Vector3 maxv(Vector3 a, Vector3 b)
{
	return (Vector3) {
		maxf(a.x, b.x),
		maxf(a.y, b.y),
		maxf(a.z, b.z),
	};
}

Vector3 vec_from_scalar(float s)
{
	return (Vector3) {s, s, s};
}

Vector3 fresnelSchlickRoughness(float cosTheta, Vector3 F0, float roughness)
{
	return combine(F0, combine(maxv(vec_from_scalar(1.0 - roughness), F0), F0, 1, -1), 1, pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0));
}

Vector3 fresnelSchlick(float u, Vector3 f0) {
    return combine(f0, combine(vec_from_scalar(1.0), f0, 1, -1), 1, pow(1.0 - u, 5.0));
}

float geometrySmith(float NoV, float NoL, float a) {
	float a2 = a * a;
	float GGXL = NoV * sqrt((-NoL * a2 + NoL) * NoL + a2);
	float GGXV = NoL * sqrt((-NoV * a2 + NoV) * NoV + a2);
	return 0.5 / (GGXV + GGXL);
}

float distribGGX(float NoH, float roughness) {
	float a = NoH * roughness;
	float k = roughness / (1.0 - NoH * NoH + a * a);
	return k * k * (1.0 / M_PI);
}

typedef struct {
	uint8_t *data[6];
	int w, h, chan;
} Cubemap;

typedef enum {
	CF_FRONT,
	CF_BACK,
	CF_LEFT,
	CF_RIGHT,
	CF_TOP,
	CF_BOTTOM,
} CubeFace;

void load_cubemap(Cubemap *c, const char *files[6])
{
	for (int i = 0; i < 6; i++) {
		c->data[i] = stbi_load(files[i], &c->w, &c->h, &c->chan, 0);
		if (c->data[i] == NULL) {
			fprintf(stderr, "Couldn't load image '%s'\n", files[i]);
			abort();
		}
	}
}

void free_cubemap(Cubemap *c)
{
	for (int i = 0; i < 6; i++) {
		stbi_image_free(c->data[i]);
	}
}

Vector3 sample_cubemap(Cubemap *c, Vector3 dir)
{
	float abs_x = absf(dir.x);
	float abs_y = absf(dir.y);
	float abs_z = absf(dir.z);

	CubeFace face;

	float u;
	float v;
	float eps = 0;

	if (abs_x > abs_y && abs_x > abs_z) {
		// X dominant
		if (dir.x > 0) {
			// right face
			face = CF_RIGHT;
			u = -dir.z / (abs_x + eps);
			v = -dir.y / (abs_x + eps);
		} else {
			// left face
			face = CF_LEFT;
			u = dir.z / (abs_x + eps);
			v = -dir.y / (abs_x + eps);
		}
	} else if (abs_y > abs_x && abs_y > abs_z) {
		// Y dominant
		assert(abs_y > 0);
		if (dir.y > 0) {
			// top face
			face = CF_TOP;
			u = dir.x / (abs_y + eps);
			v = dir.z / (abs_y + eps);
		} else {
			// bottom face
			face = CF_BOTTOM;
			u = dir.x / (abs_y + eps);
			v = -dir.z / (abs_y + eps);
		}
	} else {
		// Z dominant
		if (dir.z > 0) {
			// front face
			face = CF_FRONT;
			u = dir.x / (abs_z + eps);
			v = -dir.y / (abs_z + eps);
		} else {
			// back face
			face = CF_BACK;
			u = -dir.x / (abs_z + eps);
			v = -dir.y / (abs_z + eps);
		}
	}

	u = clamp(u, -1, 1);
	v = clamp(v, -1, 1);

	u = 0.5f * (u + 1.0f);
	v = 0.5f * (v + 1.0f);

	// Pixel coordinates
	int x = u * (c->w - 1);
	int y = v * (c->h - 1);

	uint8_t *color = &c->data[face][(y * c->w + x) * c->chan];
	return (Vector3) {
		(float) color[0] / 255,
		(float) color[1] / 255,
		(float) color[2] / 255,
	};
}

static unsigned int
compile_shader(const char *vertex_file,
               const char *fragment_file)
{
	int  success;
	char infolog[512];

	char *vertex_str = load_file(vertex_file, NULL);
	if (vertex_str == NULL) {
		fprintf(stderr, "Couldn't load file '%s'\n", vertex_file);
		return 0;
	}

	char *fragment_str = load_file(fragment_file, NULL);
	if (fragment_str == NULL) {
		fprintf(stderr, "Couldn't load file '%s'\n", fragment_file);
		free(vertex_str);
		return 0;
	}

	unsigned int vertex_shader = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vertex_shader, 1, &vertex_str, NULL);
	glCompileShader(vertex_shader);

	glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
	if(!success) {
		glGetShaderInfoLog(vertex_shader, sizeof(infolog), NULL, infolog);
		fprintf(stderr, "Couldn't compile vertex shader '%s' (%s)\n", vertex_file, infolog);
		free(vertex_str);
		free(fragment_str);
		return 0;
	}

	unsigned int fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(fragment_shader, 1, &fragment_str, NULL);
	glCompileShader(fragment_shader);

	glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
	if(!success) {
		glGetShaderInfoLog(fragment_shader, sizeof(infolog), NULL, infolog);
		fprintf(stderr, "Couldn't compile fragment shader '%s' (%s)\n", fragment_file, infolog);
		free(vertex_str);
		free(fragment_str);
		return 0;
	}

	unsigned int shader_program = glCreateProgram();
	glAttachShader(shader_program, vertex_shader);
	glAttachShader(shader_program, fragment_shader);
	glLinkProgram(shader_program);

	glGetProgramiv(shader_program, GL_LINK_STATUS, &success);
	if(!success) {
		glGetProgramInfoLog(shader_program, sizeof(infolog), NULL, infolog);
		fprintf(stderr, "Couldn't link shader program (%s)\n", infolog);
		free(vertex_str);
		free(fragment_str);
		return 0;
	}

	glDeleteShader(vertex_shader);
	glDeleteShader(fragment_shader);
	free(vertex_str);
	free(fragment_str);
	return shader_program;
}

static void set_uniform_m4(unsigned int program, const char *name, Matrix4 value)
{
	int location = glGetUniformLocation(program, name);
	if (location < 0) {
		printf("Can't set uniform '%s'\n", name);
		abort();
	}
	glUniformMatrix4fv(location, 1, GL_FALSE, (float*) &value);
}

static void set_uniform_v3(unsigned int program, const char *name, Vector3 value)
{
	int location = glGetUniformLocation(program, name);
	if (location < 0) {
		printf("Can't set uniform '%s' (program %d, location %d)\n", name, program, location);
		abort();
	}
	glUniform3f(location, value.x, value.y, value.z);
}

static void set_uniform_i(unsigned int program, const char *name, int value)
{
	int location = glGetUniformLocation(program, name);
	if (location < 0) {
		printf("Can't set uniform '%s'\n", name);
		abort();
	}
	glUniform1i(location, value);
}

static void set_uniform_f(unsigned int program, const char *name, float value)
{
	int location = glGetUniformLocation(program, name);
	if (location < 0) {
		printf("Can't set uniform '%s'\n", name);
		abort();
	}
	glUniform1f(location, value);
}

static void error_callback(int error, const char* description)
{
    fprintf(stderr, "Error: %s\n", description);
}

static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        glfwSetWindowShouldClose(window, GLFW_TRUE);
}

void framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
    glViewport(0, 0, width, height);
}

void invalidate_accumulation(void);

void cursor_pos_callback(GLFWwindow *window, double x, double y)
{
	invalidate_accumulation();
    rotate_camera(x, y);
}

typedef struct {
	Vector3 origin;
	Vector3 size;
} Cube;

bool intersect_cube(Ray r, Cube c, float *tnear, float *tfar, Vector3 *normal)
{
	float txmin, txmax;
	float tymin, tymax;
	float tzmin, tzmax;

	float tn;
	float tf;

	Vector3 a = c.origin;
	Vector3 b = combine(c.origin, c.size, 1, 1);

	int hit_axis = 0; // 0=x, 1=y, 2=z

	if (r.direction.x >= 0) {
		txmin = (a.x - r.origin.x) / r.direction.x;
		txmax = (b.x - r.origin.x) / r.direction.x;
	} else {
		txmax = (a.x - r.origin.x) / r.direction.x;
		txmin = (b.x - r.origin.x) / r.direction.x;
	}

	if (r.direction.y >= 0) {
		tymin = (a.y - r.origin.y) / r.direction.y;
		tymax = (b.y - r.origin.y) / r.direction.y;
	} else {
		tymax = (a.y - r.origin.y) / r.direction.y;
		tymin = (b.y - r.origin.y) / r.direction.y;
	}

	if (txmin > tymax || tymin > txmax)
		return false;

	if (tymin > txmin) { txmin = tymin; hit_axis = 1; }
	if (tymax < txmax) txmax = tymax;

	if (r.direction.z >= 0) {
		tzmin = (a.z - r.origin.z) / r.direction.z;
		tzmax = (b.z - r.origin.z) / r.direction.z;
	} else {
		tzmax = (a.z - r.origin.z) / r.direction.z;
		tzmin = (b.z - r.origin.z) / r.direction.z;
	}

	if (txmin > tzmax || tzmin > txmax)
		return false;
	
	if (tzmin > txmin) { txmin = tzmin; hit_axis = 2; };
	if (tzmax < txmax) txmax = tzmax;

	if (tnear) *tnear = txmin;
	if (tfar)  *tfar  = txmax;
	if (normal) {
		switch (hit_axis) {
			case 0: *normal = r.direction.x > 0 ? (Vector3) {-1, 0, 0} : (Vector3) {1, 0, 0}; break;
			case 1: *normal = r.direction.y > 0 ? (Vector3) {0, -1, 0} : (Vector3) {0, 1, 0}; break;
			case 2: *normal = r.direction.z > 0 ? (Vector3) {0, 0, -1} : (Vector3) {0, 0, 1}; break;
		}
	}
	return true;
}

bool intersect_sphere(Ray r, Sphere s, float *t)
{
	/*
	 * Any point of the ray can be written as
	 *
	 *     P(t) = O + t * D
	 * 
	 * with O origin and D direction.
	 * 
	 * All points P=(x,y,z) of a sphere can be described as
	 * those (and only those) that satisfy the equation
	 *
	 *     x^2 + y^2 + z^2 = R^2
	 *     P^2 - R^2 = 0
	 * 
	 * with R radius of the sphere. The sphere here is centered
	 * at the origin.
	 * 
	 * Intersection points of the ray with the sphere must satisfy
	 * both:
	 * 
	 *     P(t) = O + t * D
	 *     P^2 - R^2 = 0
	 * 
	 *     => (O + tD)^2 - R^2 = 0
	 *     => t^2 * D^2 + t * 2OD + O^2 - R^2 = 0
	 * 
	 * we can use the quadratic formula here, and more specifically
	 * the discriminant to check if solutions exist and how many
	 */
	Vector3 oc = combine(s.center, r.origin, 1, -1);
	float a = dotv(r.direction, r.direction);
	float b = -2 * dotv(oc, r.direction);
	float c = dotv(oc, oc) - s.radius * s.radius;

	float discr = b*b - 4*a*c;

	if (discr > 0) {
		float s0 = (- b + sqrt(discr)) / (2 * a);
		float s1 = (- b - sqrt(discr)) / (2 * a);
		if (s0 > s1) {
			float tmp = s0;
			s0 = s1;
			s1 = tmp;
		}
		if (s0 < 0) {
			s0 = s1;
			if (s0 < 0) return false;
		}
		if (t) *t = s0;
		return true;
	}

	// Zero solutions
	return false;
}

typedef enum {
	OBJECT_CUBE,
	OBJECT_SPHERE,
} ObjectType;

typedef struct {
	ObjectType type;
	union {
		Sphere sphere;
		Cube cube;
	};
	Material material;
} Object;

Object   cube(Material material, Vector3 origin, Vector3 size) { return (Object) {.material=material, .type=OBJECT_CUBE, .cube=(Cube) {.origin=origin, .size=size}}; }
Object sphere(Material material, Vector3 origin, float radius) { return (Object) {.material=material, .type=OBJECT_SPHERE, .sphere=(Sphere) {.center=origin, .radius=radius}}; }

bool intersect_object(Ray r, Object o, float *t, Vector3 *normal)
{
	switch (o.type) {

		case OBJECT_CUBE:
		return intersect_cube(r, o.cube, t, NULL, normal);

		case OBJECT_SPHERE:
		if (intersect_sphere(r, o.sphere, t)) {
			if (normal) {
				Vector3 hit_point = combine(r.origin, r.direction, 1, *t);
				*normal = normalize(combine(hit_point, o.sphere.center, 1, -1));
			}
			return true;
		}
		return false;
	}
	return false;
}

float random_float(void)
{
	return (float) rand() / RAND_MAX;
}

Vector3 random_vector(void)
{
	return (Vector3) {
		.x = random_float() * 2 - 1,
		.y = random_float() * 2 - 1,
		.z = random_float() * 2 - 1,
	};
}

Vector3 random_direction(void)
{
	return normalize(random_vector());
}

Vector3 reflect(Vector3 dir, Vector3 normal)
{
	float f = -2 * dotv(normal, dir);
	return combine(dir, normal, 1, f);
}

#define MAX_OBJECTS 1024
Object objects[MAX_OBJECTS];
int num_objects = 0;

void add_object(Object o)
{
	if (num_objects < MAX_OBJECTS)
		objects[num_objects++] = o;
}

typedef struct {
	float   distance;
	Vector3 point;
	Vector3 normal;
	int     object;
} HitInfo;

HitInfo trace_ray(Ray ray)
{
	ray.direction = normalize(ray.direction);

	float   nearest_t = FLT_MAX;
	int     nearest_object = -1;
	Vector3 nearest_normal;
	for (int i = 0; i < num_objects; i++) {
		float t;
		Vector3 n;
		if (!intersect_object(ray, objects[i], &t, &n))
			continue;
		if (t >= 0 && t < nearest_t) {
			nearest_t = t;
			nearest_object = i;
			nearest_normal = n;
		}
	}

	if (nearest_object == -1) {
		HitInfo result;
		result.distance = -1;
		result.normal   = (Vector3) {0, 0, 0};
		result.point    = (Vector3) {0, 0, 0};
		result.object   = -1;
		return result;
	} else {
		HitInfo result;
		result.distance = nearest_t;
		result.normal   = nearest_normal;
		result.point    = combine(ray.origin, ray.direction, 1, nearest_t);
		result.object   = nearest_object;
		return result;
	}
}

Vector3 origin_of(Object o)
{
	if (o.type == OBJECT_SPHERE)
		return o.sphere.center;
	return combine(o.cube.origin, o.cube.size, 1, 0.5);
}

Cubemap skybox;

Vector3 F_Schlick(float u, Vector3 f0)
{
    float f = pow(1.0 - u, 5.0);
    return combine(vec_from_scalar(f), f0, 1, (1.0 - f));
}

bool iszerof(float f)
{
	return f < 0.0001 && f > -0.0001;
}

bool iszerov(Vector3 v)
{
	return iszerof(v.x) && iszerof(v.y) && iszerof(v.z);
}

float avgv(Vector3 v)
{
	return (v.x + v.y + v.z) / 3;
}

Vector3 pixel(float x, float y, float aspect_ratio)
{
	assert(!isnan(aspect_ratio));

	Ray in_ray = ray_through_screen_at(x, y, aspect_ratio);
	assert(!isnanv(in_ray.direction));

	//Vector3 sky_color = {0.6, 0.7, 0.9};
	//Vector3 sky_color = {0, 0, 0};
	//Vector3 sky_color = {1, 1, 1};

	Vector3 contrib = {1, 1, 1};
	Vector3 result = {0, 0, 0};
	for (int i = 0; i < 5; i++) {

		HitInfo hit = trace_ray(in_ray);
		if (hit.object == -1) {
			Vector3 sky_color = sample_cubemap(&skybox, normalize(in_ray.direction));
			result = combine(result, mulv(sky_color, contrib), 1, 1);
			break;
		}

		Vector3 sampled_light_color = {0, 0, 0};
		for (int j = 0; j < num_objects; j++) {
			if (objects[j].material.emission_power == 0 || j == hit.object)
				continue;
			Vector3 dir_to_light_source = combine(origin_of(objects[j]), hit.point, 1, -1);
			int samples = 5;
			float spread = 0.5;
			for (int k = 0; k < samples; k++) {
				// Add some noise based on roughness
				Vector3 rand_dir = random_direction();
				if (dotv(rand_dir, hit.normal) < 0)
					rand_dir = scale(rand_dir, -1);
				Vector3 sample_dir = normalize(combine(rand_dir, dir_to_light_source, spread, 1));
				Ray sample_ray = { combine(hit.point, sample_dir, 1, 0.001), sample_dir };
				HitInfo hit2 = trace_ray(sample_ray);
				if (hit2.object != -1)
					sampled_light_color = combine(sampled_light_color, objects[hit2.object].material.emission_color, 1, objects[hit2.object].material.emission_power);
			}
			sampled_light_color = scale(sampled_light_color, 1.0f / samples);
			break;
		}

		Material material = objects[hit.object].material;

		Vector3 v = scale(in_ray.direction, -1);
		//Vector3 l = out_dir;
		Vector3 n = hit.normal;
		//Vector3 h = normalize(combine(v, l, 1, 1));
		//float NoH = clamp(dotv(n, h), 0, 1);
		//float LoH = clamp(dotv(l, h), 0, 1);
		float NoV = clamp(dotv(n, v), 0, 1);
		//float NoL = clamp(dotv(n, l), 0, 1);

		Vector3 f0_dielectric = vec_from_scalar(0.16 * material.reflectance * material.reflectance);
		Vector3 f0_metal = material.albedo;
		Vector3 f0 = combine(f0_dielectric, f0_metal, (1 - material.metallic), material.metallic);
		Vector3 F = fresnelSchlick(NoV, f0);

		Vector3 rand_dir = random_direction();
		if (dotv(rand_dir, hit.normal) < 0)
			rand_dir = scale(rand_dir, -1);

		result = combine(result, mulv(scale(material.emission_color, material.emission_power), contrib), 1, 1);

		Vector3 out_dir;
		if (material.metallic > 0.001 || random_float() <= avgv(F)) {
			// Specular ray
			Vector3 reflect_dir = reflect(in_ray.direction, scale(hit.normal, -1));
			out_dir = normalize(combine(rand_dir, reflect_dir, material.roughness, 1));
		} else {
			// Diffuse ray
			out_dir = rand_dir;
			contrib = mulv(contrib, scale(material.albedo, (1 - material.metallic)));
		}
		Ray out_ray = { combine(hit.point, out_dir, 1, 0.001), out_dir };

		float light_sample_weight = 0.05;
		if (!iszerov(sampled_light_color)) {
			result = combine(result, mulv(sampled_light_color, contrib), 1, light_sample_weight);
			contrib = scale(contrib, 1 - light_sample_weight);
		}

		in_ray = out_ray;
	}

	result.x = clamp(result.x, 0, 1);
	result.y = clamp(result.y, 0, 1);
	result.z = clamp(result.z, 0, 1);

	return result;
}

uint32_t accum_generation = 0;
Vector3 *accum = NULL;
Vector3 *frame = NULL;
int      frame_w = 0;
int      frame_h = 0;
unsigned int frame_texture;
float accum_count = 0;
os_mutex_t frame_mutex;

os_threadreturn worker(void *arg)
{
	uint32_t local_accum_generation = 0;
	Vector3 *local_accum = NULL;
	float    local_accum_count = 0;
	int local_frame_w = 0;
	int local_frame_h = 0;

	int init_scale = (int) arg;
	int scale = init_scale;

	for (;;) {

		bool resize = false;

		os_mutex_lock(&frame_mutex);
		if (accum != NULL && local_accum != NULL && local_accum_generation == accum_generation) {
			for (int i = 0; i < frame_w * frame_h; i++)
				accum[i] = combine(accum[i], local_accum[i], 1, 1);
			accum_count += local_accum_count;
			//if (scale > 1)
			//	scale >>= 1;
		} else {
			//scale = init_scale;
		}
		memset(local_accum, 0, sizeof(Vector3) * local_frame_w * local_frame_h);
		if (local_frame_w != frame_w || local_frame_h != frame_h)
			resize = true;
		local_accum_generation = accum_generation;
		local_frame_w = frame_w;
		local_frame_h = frame_h;
		local_accum_count = 0;
		os_mutex_unlock(&frame_mutex);

		if (resize) {

			if (local_accum)
				free(local_accum);

			local_accum = malloc(sizeof(Vector3) * local_frame_w * local_frame_h);
			if (!local_accum) {
				printf("OUT OF MEMORY\n");
				abort();
			}

			memset(local_accum, 0, sizeof(Vector3) * local_frame_w * local_frame_h);
		}

		if (local_accum) {

			float scale2inv = 1.0f / (scale * scale);

			int lowres_frame_h = local_frame_h / scale;
			int lowres_frame_w = local_frame_w / scale;
			float aspect_ratio = (float) local_frame_w / local_frame_h;

			for (int j = 0; j < lowres_frame_h; j++)
				for (int i = 0; i < lowres_frame_w; i++) {
					float u = (float) i / (lowres_frame_w - 1);
					float v = (float) j / (lowres_frame_h - 1);
					u = 1 - u;
					v = 1 - v;

					int tile_w = scale;
					int tile_h = scale;
					if (tile_w > local_frame_w - i * scale) tile_w = local_frame_w - i * scale;
					if (tile_h > local_frame_h - j * scale) tile_h = local_frame_h - j * scale;

					Vector3 color = pixel(u, v, aspect_ratio);
					for (int g = 0; g < tile_h; g++)
						for (int t = 0; t < tile_w; t++) {
							int real_i = i * scale + t;
							int real_j = j * scale + g;
							int pixel_index = real_j * frame_w + real_i;
							local_accum[pixel_index] = combine(local_accum[pixel_index], color, 1, scale2inv);
						}
				}

			local_accum_count += scale2inv;
		}
	}
}

void invalidate_accumulation(void)
{
	os_mutex_lock(&frame_mutex);
	accum_count = 0;
	accum_generation++;
	os_mutex_unlock(&frame_mutex);
}

void update_frame_texture(float s)
{
	os_mutex_lock(&frame_mutex);

	if (frame_w != s * screen_w || frame_h != s * screen_h) {
		frame_w = s * screen_w;
		frame_h = s * screen_h;

		if (frame) free(frame);
		if (accum) free(accum);

		frame = malloc(sizeof(Vector3) * frame_w * frame_h);
		if (!frame) { printf("OUT OF MEMORY\n"); abort(); }

		accum = malloc(sizeof(Vector3) * frame_w * frame_h);
		if (!accum) { printf("OUT OF MEMORY\n"); abort(); }

		accum_count = 0;
	}

	if (accum_count == 0) {

		int scale_ = 16;
		float scale2inv = 1.0f / (scale_ * scale_);

		int fake_frame_w = frame_w / scale_;
		int fake_frame_h = frame_h / scale_;
		float aspect_ratio = (float) fake_frame_w/fake_frame_h;

		for (int j = 0; j < fake_frame_h; j++)
			for (int i = 0; i < fake_frame_w; i++) {
				float u = (float) i / (fake_frame_w - 1);
				float v = (float) j / (fake_frame_h - 1);
				u = 1 - u;
				v = 1 - v;
				Vector3 color = pixel(u, v, aspect_ratio);
				for (int g = 0; g < scale_; g++)
					for (int t = 0; t < scale_; t++) {
						int pixel_index = (j * scale_ + g) * frame_w + (i * scale_ + t);
						accum[pixel_index] = scale(color, scale2inv);
					}
			}

		accum_count += scale2inv;
	}

	for (int j = 0; j < frame_h; j++)
		for (int i = 0; i < frame_w; i++) {

			float u = (float) i / (frame_w - 1);
			float v = (float) j / (frame_h - 1);
			u = 1 - u;
			v = 1 - v;

			int pixel_index = j * frame_w + i;
			frame[pixel_index] = scale(accum[pixel_index], 1.0f / accum_count);
		}

	glBindTexture(GL_TEXTURE_2D, frame_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, frame_w, frame_h, 0, GL_RGB, GL_FLOAT, frame);
	glBindTexture(GL_TEXTURE_2D, 0);

	os_mutex_unlock(&frame_mutex);
}

int main(void)
{
#if 0

	add_object(sphere(
		(Material) {
			.emission_color={1, 1, 1},
			.emission_power=1,
			.metallic=0,
			.reflectance=0,
			.roughness=0,
			.albedo=(Vector3) {0.2, 0.5, 1},
		},
		(Vector3) {0, 2, 0},
		1)
	);

	add_object(sphere(
		(Material) {
			.emission_color={0},
			.emission_power=0,
			.metallic=0,
			.reflectance=0,
			.roughness=0,
			.albedo=(Vector3) {0.2, 0.5, 1},
		},
		(Vector3) {0, 0, 3},
		1)
	);

	add_object(sphere(
		(Material) {
			.emission_color={0},
			.emission_power=0,
			.metallic=1,
			.reflectance=0,
			.roughness=0,
			.albedo=(Vector3) {0.5, 0.2, 1},
		},
		(Vector3) {3, 0, 0},
		1)
	);

#elif 0

	add_object(sphere(
		(Material) {
			.emission_color={0},
			.emission_power=0,
			.metallic=0,
			.reflectance=1,
			.roughness=0,
			.albedo=(Vector3) {0.2, 0.5, 1},
		},
		(Vector3) {-3, 0, 0},
		1)
	);

	add_object(sphere(
		(Material) {
			.emission_color={0},
			.emission_power=0,
			.metallic=0,
			.reflectance=0,
			.roughness=0,
			.albedo=(Vector3) {0.2, 0.5, 1},
		},
		(Vector3) {0, 0, 0},
		1)
	);

	add_object(sphere(
		(Material) {
			.emission_color={0},
			.emission_power=0,
			.metallic=1,
			.reflectance=0,
			.roughness=0,
			.albedo=(Vector3) {0.5, 0.2, 1},
		},
		(Vector3) {3, 0, 0},
		1)
	);

#elif 0
	int num_spheres = 5;
	for (int i = 0; i < num_spheres; i++) {
		add_object(sphere(
			(Material) {
				.emission_color={0},
				.emission_power=0,
				.metallic=0,
				.reflectance=0,
				.roughness= (float) i / (num_spheres-1),
				.albedo=(Vector3) {0.2, 0.5, 1},
			},
			(Vector3) {3 * i, 0, 0},
			1)
		);
		add_object(sphere(
			(Material) {
				.emission_color={0},
				.emission_power=0,
				.metallic=1,
				.reflectance=0,
				.roughness= (float) i / (num_spheres-1),
				.albedo=(Vector3) {0.2, 0.5, 1},
			},
			(Vector3) {3 * i, 3, 0},
			1)
		);
	}
#elif 0
	float box_d = 3;
	float box_w = 3;
	float box_h = 5;
	float box_border = 0.1;

	add_object(cube(
		(Material) {
			.emission_color={0},
			.emission_power=0,
			.roughness=1,
			.metallic=0,
			.reflectance=0,
			.albedo=(Vector3) {1, 0.3, 0.3}
		}, 
		(Vector3) {0, 0, 0}, 
		(Vector3) {box_w, box_border, box_d}
	));

	add_object(cube(
		(Material) {
			.emission_color={0},
			.emission_power=0,
			.metallic=0,
			.reflectance=0,
			.roughness=1,
			.albedo=(Vector3) {0.3, 1, 0.3}
		}, 
		(Vector3) {0, box_h, 0}, 
		(Vector3) {box_w, box_border, box_d}
	));

	add_object(cube(
		(Material) {
			.emission_color={0},
			.emission_power=0,
			.metallic=0,
			.reflectance=0,
			.roughness=1,
			.albedo=(Vector3) {0.3, 0.3, 1}
		},
		(Vector3) {0, 0, 0}, 
		(Vector3) {box_border, box_h, box_d}
	));

	add_object(cube(
		(Material) {
			.emission_color={0},
			.emission_power=0,
			.metallic=0,
			.reflectance=0,
			.roughness=1,
			.albedo=(Vector3) {0.3, 1, 1}
		}, 
		(Vector3) {box_w, 0, 0},
		(Vector3) {box_border, box_h, box_d}
	));

	add_object(cube(
		(Material) {
			.emission_color={0},
			.emission_power=0,
			.metallic=0,
			.reflectance=0,
			.roughness=0,
			.albedo=(Vector3) {1, 0.3, 1}
		},
		(Vector3) {0, 0, 0}, 
		(Vector3) {box_w, box_h, box_border}
	));

	add_object(cube(
		(Material) {
			.emission_color={1, 1, 1},
			.emission_power=1,
			.metallic=0,
			.reflectance=0,
			.roughness=0,
			.albedo=(Vector3) {1, 1, 0.3}
		}, 
		(Vector3) {box_w/3, box_h-box_border, box_d/3}, 
		(Vector3) {box_w/3, box_border, box_d/3}
	));

	add_object(sphere(
		(Material) {
			.emission_color={0},
			.emission_power=0,
			.metallic=1,
			.reflectance=0,
			.roughness=0.5,
			.albedo=(Vector3) {0, 1, 0}
		},
		(Vector3) {box_w/2, box_w/3, box_d/2}, 
		box_w/3
	));

#elif 1
	add_object(cube  ((Material) {.emission_color={0},       .emission_power=0, .metallic=1, .reflectance=0, .roughness=1,   .albedo=(Vector3) {1, 0.3, 0.3}},   (Vector3) {0, 0, 0},    (Vector3) {3, 5, 0.1}));
	add_object(cube  ((Material) {.emission_color={0},       .emission_power=0, .metallic=1, .reflectance=0, .roughness=0.5, .albedo=(Vector3) {1, 0.3, 0.3}},   (Vector3) {3, 0, 0},    (Vector3) {3, 5, 0.1}));
	add_object(cube  ((Material) {.emission_color={0},       .emission_power=0, .metallic=1, .reflectance=0, .roughness=0,   .albedo=(Vector3) {1, 0.3, 0.3}},   (Vector3) {6, 0, 0},    (Vector3) {3, 5, 0.1}));
/*
	add_object(cube  ((Material) {.emission_color={0},       .emission_power=0, .metallic=0, .reflectance=0, .roughness=1,   .albedo=(Vector3) {0.3, 1, 0.3}},   (Vector3) {0, 0, 0},    (Vector3) {0.1, 5, 3}));
	add_object(cube  ((Material) {.emission_color={0},       .emission_power=0, .metallic=0, .reflectance=0, .roughness=0.5, .albedo=(Vector3) {0.3, 1, 0.3}},   (Vector3) {0, 0, 3},    (Vector3) {0.1, 5, 3}));
	add_object(cube  ((Material) {.emission_color={0},       .emission_power=0, .metallic=0, .reflectance=0, .roughness=0,   .albedo=(Vector3) {0.3, 1, 0.3}},   (Vector3) {0, 0, 6},    (Vector3) {0.1, 5, 3}));
*/
	add_object(cube  ((Material) {.emission_color={0},       .emission_power=0, .metallic=0, .reflectance=0, .roughness=1, .albedo=(Vector3) {0.4, 0.3, 0.9}}, (Vector3) {0, -0.1, 0}, (Vector3) {9, 0.1, 9}));
	
	add_object(cube  ((Material) {.emission_color={0},       .emission_power=0, .metallic=0, .reflectance=0, .roughness=1,   .albedo=(Vector3) {1, 0, 0}},       (Vector3) {5, 0, 6},    (Vector3) {1, 1, 1}));
	add_object(cube  ((Material) {.emission_color={0},       .emission_power=0, .metallic=0, .reflectance=1, .roughness=0,   .albedo=(Vector3) {1, 0, 1}},       (Vector3) {4, 0, 5},    (Vector3) {1, 1, 1}));
	
	add_object(sphere((Material) {.emission_color={0},       .emission_power=0, .metallic=0, .reflectance=0, .roughness=1, .albedo=(Vector3) {1, 0.4, 0}},     (Vector3) {3, 1, 3}, 1));
	add_object(sphere((Material) {.emission_color={0},       .emission_power=0, .metallic=0, .reflectance=1, .roughness=0,   .albedo=(Vector3) {0, 1, 0}},       (Vector3) {5, 1, 3}, 1));
	add_object(sphere((Material) {.emission_color={1, 0.5, 0.5}, .emission_power=5, .metallic=0, .reflectance=0, .roughness=1,   .albedo=(Vector3) {1, 0.4, 0}},     (Vector3) {3, 5, 3}, 1));
#elif 0
//	add_object(cube  ((Material) {.emission_color={0},           .emission_power=0, .metallic=0, .reflectance=0, .roughness=0,   .albedo=(Vector3) {1, 0.3, 0.3}},   (Vector3) {0, 0, 0},    (Vector3) {10, 5, 0.1}));
//	add_object(cube  ((Material) {.emission_color={0},           .emission_power=0, .metallic=0, .reflectance=0, .roughness=0.6, .albedo=(Vector3) {0.3, 1, 0.3}},   (Vector3) {0, 0, 0},    (Vector3) {0.1, 5, 10}));
	add_object(cube  ((Material) {.emission_color={0},           .emission_power=0, .metallic=0, .reflectance=0, .roughness=1, .albedo=(Vector3) {0.4, 0.3, 0.9}}, (Vector3) {0, -0.1, 0}, (Vector3) {10, 0.1, 10}));
//	add_object(cube  ((Material) {.emission_color={0},           .emission_power=0, .metallic=0, .reflectance=0, .roughness=1,   .albedo=(Vector3) {1, 0, 0}},       (Vector3) {7, 0, 8},    (Vector3) {1, 1, 1}));
//	add_object(cube  ((Material) {.emission_color={0},           .emission_power=0, .metallic=0, .reflectance=0, .roughness=0,   .albedo=(Vector3) {1, 0, 1}},       (Vector3) {6, 0, 7},    (Vector3) {1, 1, 1}));
//	add_object(sphere((Material) {.emission_color={0},           .emission_power=0, .metallic=0, .reflectance=0, .roughness=0.5, .albedo=(Vector3) {1, 0.4, 0}},     (Vector3) {3, 1, 3}, 1));
//	add_object(sphere((Material) {.emission_color={0},           .emission_power=0, .metallic=0, .reflectance=0, .roughness=0,   .albedo=(Vector3) {0, 1, 0}},       (Vector3) {5, 1, 3}, 1));
	add_object(sphere((Material) {.emission_color={1, 1, 1}, .emission_power=1, .metallic=0, .reflectance=0, .roughness=1,   .albedo=(Vector3) {1, 0.4, 0}},     (Vector3) {3, 5, 3}, 1));
#endif


	{
		const char *faces[] = {
			[CF_RIGHT]  = "assets/skybox/right.jpg",
			[CF_LEFT]   = "assets/skybox/left.jpg",
			[CF_TOP]    = "assets/skybox/top.jpg",
			[CF_BOTTOM] = "assets/skybox/bottom.jpg",
			[CF_FRONT]  = "assets/skybox/front.jpg",
			[CF_BACK]   = "assets/skybox/back.jpg",
		};
		load_cubemap(&skybox, faces);
	}

    glfwSetErrorCallback(error_callback);

    if (!glfwInit())
        return -1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

	int window_w = 2 * 640;
	int window_h = 2 * 480;
    GLFWwindow *window = glfwCreateWindow(window_w, window_h, "Path Trace", NULL, NULL);
    if (!window) {
        glfwTerminate();
        return -1;
    }

    glfwSetKeyCallback(window, key_callback);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetCursorPosCallback(window, cursor_pos_callback);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    glfwMakeContextCurrent(window);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        printf("Failed to initialize GLAD\n");
        return -1;
    }

    glfwSwapInterval(1);

	glfwGetWindowSize(window, &screen_w, &screen_h);

	os_mutex_create(&frame_mutex);

	os_thread workers[16];
	int num_workers = 0;

	for (int i = 0; i < 16; i++) {
		int init_scale = 1 << i;
		if (init_scale > 16)
			init_scale = 1;
		os_thread_create(&workers[i], (void*) init_scale, worker);
		num_workers++;
	}

	unsigned int screen_program = compile_shader("assets/screen.vs", "assets/screen.fs");
	if (!screen_program) { printf("Couldn't compile program\n"); return -1; }
	set_uniform_i(screen_program, "screenTexture", 0);

	unsigned int vao, vbo;
	{
		float vertices[] = {
			// positions   // texCoords
			-1.0f,  1.0f,  0.0f, 1.0f,
			-1.0f, -1.0f,  0.0f, 0.0f,
			1.0f, -1.0f,   1.0f, 0.0f,

			-1.0f,  1.0f,  0.0f, 1.0f,
			1.0f, -1.0f,   1.0f, 0.0f,
			1.0f,  1.0f,   1.0f, 1.0f
		};

		glGenVertexArrays(1, &vao);
		glGenBuffers(1, &vbo);

		glBindVertexArray(vao);

		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), &vertices, GL_STATIC_DRAW);

		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
	}

	glGenTextures(1, &frame_texture);
	glBindTexture(GL_TEXTURE_2D, frame_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	while (!glfwWindowShouldClose(window)) {

		glfwGetWindowSize(window, &screen_w, &screen_h);

		float speed = 0.5;
		if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) { move_camera(UP, speed); invalidate_accumulation(); }
		if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) { move_camera(DOWN, speed); invalidate_accumulation(); }
		if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) { move_camera(LEFT, speed); invalidate_accumulation(); }
		if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) { move_camera(RIGHT, speed); invalidate_accumulation(); }

		Vector3 clear_color = {1, 1, 1};

		update_frame_texture(1);

		glViewport(0, 0, screen_w, screen_h);
		glClearColor(clear_color.x, clear_color.y, clear_color.z, 1.0f);
		glClearStencil(0);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

		glUseProgram(screen_program);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, frame_texture);
		glBindVertexArray(vao);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glBindVertexArray(0);

		glfwSwapBuffers(window);
		glfwPollEvents();
	}

	free_cubemap(&skybox);
	glfwDestroyWindow(window);
	glfwTerminate();
	return 0;
}
