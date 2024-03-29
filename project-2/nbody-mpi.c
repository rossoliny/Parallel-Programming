#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <mpi.h>
/* Constants */

#define Default_Debug_Acceleration_Scale 100.0f
/* Types */

typedef struct _body {
    float x, y;
    float ax, ay;
    float vx, vy;
    float mass;
} body_t;

/* Globals */

static float debug_acceleration_scale =
    Default_Debug_Acceleration_Scale;

static body_t* bodies = NULL;
static body_t* local_bodies = NULL;
static float initial_body_mass;

static float simulation_softening_length_squared;
static float* accelerations = NULL;

/* Utilities */

#ifdef DEBUG

#include <unistd.h>
#include <stdbool.h>

static const char progress_bar[] =
    "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||";
static const int progress_bar_width =
    sizeof(progress_bar) / sizeof(progress_bar[0]);

static bool is_stdout_redirected()
{
    return !isatty(fileno(stdout));
}

static void print_progress_bar(float percentage)
{
    if (!is_stdout_redirected()) { return; }

    int value = (int) (percentage * 100.0f);
    int left_pad = (int) (percentage * progress_bar_width);
    int right_pad = progress_bar_width - left_pad;
    fprintf(
        stderr,
        "\r%3d%% [%.*s%*s]",
        value,
        left_pad,
        progress_bar,
        right_pad,
        ""
    );
    if (value == 100) {
        fprintf(stderr, " \n");
    }
    fflush(stderr);
}

#endif // DEBUG

static float unit_random()
{
    return (float) rand() / (float) RAND_MAX;
}
// Create MPI Datatype

static MPI_Datatype create_body_t()
{
    static const int count = 7;
    const int block_lengths[] = {
        1, 1,
        1, 1,
        1, 1,
        1
    };
    MPI_Aint offsets[] = {
        offsetof(body_t, x),  offsetof(body_t, y),
        offsetof(body_t, ax), offsetof(body_t, ay),
        offsetof(body_t, vx), offsetof(body_t, vy),
        offsetof(body_t, mass)
    };
    const MPI_Datatype types[] = {
        MPI_FLOAT, MPI_FLOAT,
        MPI_FLOAT, MPI_FLOAT,
        MPI_FLOAT, MPI_FLOAT,
        MPI_FLOAT
    };

    MPI_Datatype type;
    MPI_Type_create_struct(count, block_lengths, offsets, types, &type);
    MPI_Type_commit(&type);

    return type;
}


/* Simulation */

static void generate_debug_data(body_t *bodies, size_t body_count)
{
    for (size_t i = 0; i < body_count; ++i) {
        body_t *body = &bodies[i];

        float angle =
            (float) i / body_count * 2.0f * M_PI +
                (unit_random() - 0.5f) * 0.5f;

        body->x = unit_random();
        body->y = unit_random();
        body->ax = 0.0f;
        body->ay = 0.0f;
        body->mass = initial_body_mass * (unit_random() + 0.5f);

        float random = unit_random();
        body->vx = cosf(angle) * debug_acceleration_scale * random;
        body->vy = sinf(angle) * debug_acceleration_scale * random;
    }
}

static void calculate_newton_gravity_acceleration(
                body_t *first_body, body_t *second_body,
                float *ax, float *ay
            )
{
    *ax = *ay = 0.0f;

    float galactic_plane_r_x =
        second_body->x - first_body->x;
    float galactic_plane_r_y =
        second_body->y - first_body->y;

    float distance_squared =
        (galactic_plane_r_x * galactic_plane_r_x  +
         galactic_plane_r_y * galactic_plane_r_y) +
            simulation_softening_length_squared;
    float distance_squared_cubed =
        distance_squared * distance_squared * distance_squared;
    float inverse =
        1.0f / sqrtf(distance_squared_cubed);
    float scale =
        second_body->mass * inverse;

    *ax += galactic_plane_r_x * scale;
    *ay += galactic_plane_r_y * scale;
}

static void integrate(body_t *body, float delta_time)
{
    body->vx += body->ax * delta_time;
    body->vy += body->ay * delta_time;
    body->x  += body->vx * delta_time;
    body->y  += body->vy * delta_time;
}

int main(int argc, char **argv)
{
	int exit_status = EXIT_SUCCESS;
	
	MPI_Init(&argc, &argv);
	int w_size, rank;
	
	MPI_Comm_size(MPI_COMM_WORLD, &w_size);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);

	MPI_Datatype mpi_body_t = create_body_t();

    if (argc < 6) {
        fprintf(
            stderr,
            "Error: incorrect number of arguments\n\n"
            "\tUsage: %s <time period (~10-100)> "
                        "<delta time (~0.01-0.1)> "
                        "<body count (~100-1000)> "
                        "<initial body mass (~10000)> "
                        "<softening length (~100)> "
                        "[debug acceleration scale (~100)]\n",
            argv[0]
        );

        exit_status = EXIT_FAILURE;
		goto end;
    }

    float time_period = strtof(argv[1], NULL);
    float delta_time  = strtof(argv[2], NULL);
    static const int Base = 10;
    size_t body_count = (size_t) strtol(argv[3], NULL, Base);
    initial_body_mass = strtof(argv[4], NULL);
    float softening_length = strtof(argv[5], NULL);
    simulation_softening_length_squared = softening_length * softening_length;

    if (argc > 6) {
        debug_acceleration_scale = strtof(argv[6], NULL);
    }

    bodies = (body_t *) malloc(sizeof(*bodies) * body_count);
    assert(bodies != NULL);

	if(rank == 0)
	{
	    generate_debug_data(bodies, body_count);

		printf("%zu\n%f\n%f\n", body_count, time_period, delta_time);
		for (size_t i = 0; i < body_count; ++i) {
			body_t *body = &bodies[i];
			printf(
				"%f %f\n"
				"%f %f\n"
				"%f %f\n"
				"%f\n",
				body->x,  body->y,
				body->ax, body->ay,
				body->vx, body->vy,
				body->mass
			);
		}
	}

	assert(body_count % w_size == 0);

	size_t bodies_per_process = body_count / w_size;
	local_bodies = malloc(sizeof(*local_bodies) * bodies_per_process);
	assert(local_bodies != NULL);

    size_t iterations = time_period / delta_time;
    size_t acceleration_entries = iterations * body_count * 2;
	if(rank == 0)
	{
	    accelerations = (float *) malloc(sizeof(*accelerations) * acceleration_entries);
	    assert(accelerations != NULL);
	}

	// BROADCAST
	MPI_Bcast(bodies, body_count, mpi_body_t, 0, MPI_COMM_WORLD);
	size_t slice_start = rank * bodies_per_process;
	//memcpy(local_bodies, bodies + slice_start, sizeof(*local_bodies) * bodies_per_process);
	
    for (size_t k = 0, next_acceleration = 0; k < iterations; ++k) {
#ifdef DEBUG
		if(rank == 0)
		{
        	print_progress_bar((float) (k + 1) / iterations);
		}
#endif

		body_t* p = local_bodies;
        for (size_t i = slice_start; i < slice_start + bodies_per_process; ++i) {
            body_t *first_body = &bodies[i];

            float total_ax = 0.0f, total_ay = 0.0f;
            for (size_t j = 0; j < body_count; ++j) {
                body_t *second_body = &bodies[j];
                if (first_body == second_body) {
                    continue;
                }

                float ax, ay;
                calculate_newton_gravity_acceleration(
                    first_body, second_body,
                    &ax, &ay
                );

                total_ax += ax;
                total_ay += ay;
            }

            first_body->ax = total_ax;
            first_body->ay = total_ay;

			*p++ = *first_body;
        }

		
        for (size_t i = 0; i <  bodies_per_process; ++i) {
            body_t *body = &local_bodies[i];

            integrate(body, delta_time);
        }
		
		// GATHER
		MPI_Allgather(local_bodies, bodies_per_process, mpi_body_t, bodies, bodies_per_process, mpi_body_t, MPI_COMM_WORLD);
		
		//memcpy(local_bodies, bodies + slice_start, sizeof(*local_bodies) * bodies_per_process );

		if(rank == 0)
		{
			for(int i = 0; i < body_count; ++i)
			{
				body_t *body = &bodies[i];

				accelerations[next_acceleration++] = body->ax;
				accelerations[next_acceleration++] = body->ay;
			}
		}
    }
		

	if(rank == 0)
	{
		for (size_t i = 0; i < acceleration_entries; i += 2) {
			printf("%f %f\n", accelerations[i], accelerations[i + 1]);
		}
	}

end:
	MPI_Finalize();

	if(!accelerations)
	{
    	free(accelerations);
    	accelerations = NULL;
	}
	
	if(!bodies)
	{
    	free(bodies);
	    bodies = NULL;
	}

    return exit_status;
}

