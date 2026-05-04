#include <ncurses.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

#define UPDATE_INTERVAL 15000 // (15ms)
#define MAX_RAIN 2000
#define MAX_BOLTS 5
#define MAX_CATS 5
#define CAT_WIDTH 7
#define JUMP_FRAMES 18
#define JUMP_STEP 5     // update cycles per animation frame (~75ms/frame, slow and lazy)
#define CAT_COOLDOWN 120 // min update cycles between jumps

// Gentle hop arc — only 4 rows at peak, feels soft
static const int jump_offsets[JUMP_FRAMES] = {0, 1, 2, 3, 4, 4, 4, 3, 3, 2, 2, 1, 1, 1, 1, 0, 0, 0};

// Rain must cross this many rows above the cat bottom to trigger a jump
#define JUMP_TRIGGER_DIST 10

typedef struct {
    int x;
    float y;
    float speed;
    char ch;
} Raindrop;

typedef struct {
    int y, x;
    double created;
} Segment;

typedef struct {
    Segment segments[1000];
    int seg_count;
    int growing;
    int target_len;
    int max_y, max_x;
    double last_growth;
} LightningBolt;

typedef enum { CAT_SIT, CAT_WALK_L, CAT_WALK_R } CatState;

typedef struct {
    float x;
    CatState state;
    int walk_timer;
    int jumping;        // 1 if in jump animation
    int jump_frame;     // index into jump_offsets[]
    int jump_step;      // sub-frame cycle counter
    int cooldown;       // remaining cycles before next jump allowed
    float jump_start_x; // x when jump began
    int jump_dx;        // total horizontal displacement across the arc
} Cat;

int rows, cols;
int is_thunderstorm = 0;
int rain_count = 0;
int bolt_count = 0;
int cat_count = 0;
int cats_enabled = 0;
int cat_count_override = -1; // -1 = auto

Raindrop raindrops[MAX_RAIN];
LightningBolt bolts[MAX_BOLTS];
Cat cats[MAX_CATS];

int COLOR_PAIR_RAIN_NORMAL = 1;
int COLOR_PAIR_LIGHTNING   = 2;
int COLOR_PAIR_CAT         = 3;

double get_time_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void init_colors(short rain_fg, short lightning_fg, short cat_fg) {
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(COLOR_PAIR_RAIN_NORMAL, rain_fg, -1);
        init_pair(COLOR_PAIR_LIGHTNING, lightning_fg, -1);
        init_pair(COLOR_PAIR_CAT, cat_fg, -1);
    }
}

short parse_color(const char *name) {
    if (strcasecmp(name, "black") == 0) return COLOR_BLACK;
    if (strcasecmp(name, "red") == 0) return COLOR_RED;
    if (strcasecmp(name, "green") == 0) return COLOR_GREEN;
    if (strcasecmp(name, "yellow") == 0) return COLOR_YELLOW;
    if (strcasecmp(name, "blue") == 0) return COLOR_BLUE;
    if (strcasecmp(name, "magenta") == 0) return COLOR_MAGENTA;
    if (strcasecmp(name, "cyan") == 0) return COLOR_CYAN;
    if (strcasecmp(name, "white") == 0) return COLOR_WHITE;
    return COLOR_CYAN; // default
}

void add_raindrop() {
    if (rain_count >= MAX_RAIN) return;
    Raindrop d;
    d.x = rand() % cols;
    d.y = 0;
    float min_speed = is_thunderstorm ? 0.3f : 0.3f;
    float max_speed = is_thunderstorm ? 1.0f : 0.6f;
    d.speed = ((float)rand() / RAND_MAX) * (max_speed - min_speed) + min_speed;
    char rain_chars[] = {'|', '.', '`'};
    d.ch = rain_chars[rand() % 3];
    raindrops[rain_count++] = d;
}

void update_rain() {
    int i, j = 0;
    for (i = 0; i < rain_count; i++) {
        float old_y = raindrops[i].y;
        raindrops[i].y += raindrops[i].speed;
        float new_y = raindrops[i].y;

        // Trigger a jump when rain crosses the detection threshold above a cat
        int trigger_row = rows - 1 - JUMP_TRIGGER_DIST;
        if (trigger_row >= 0 && (int)old_y < trigger_row && (int)new_y >= trigger_row) {
            for (int ci = 0; ci < cat_count; ci++) {
                Cat *c = &cats[ci];
                if (c->jumping || c->cooldown > 0) continue;
                int cx = (int)c->x;
                if (raindrops[i].x >= cx && raindrops[i].x < cx + CAT_WIDTH) {
                    // Small, gentle arc: 2-6 cols wide, random direction
                    int mag = 2 + rand() % 5;
                    int dx = (rand() % 2) ? mag : -mag;
                    // Clamp landing position to screen
                    float land = c->x + dx;
                    if (land < 0) dx = -(int)c->x;
                    if (land + CAT_WIDTH >= cols) dx = cols - CAT_WIDTH - 1 - (int)c->x;
                    c->jumping = 1;
                    c->jump_frame = 0;
                    c->jump_step = 0;
                    c->cooldown = CAT_COOLDOWN;
                    c->jump_start_x = c->x;
                    c->jump_dx = dx;
                }
            }
        }

        if ((int)new_y < rows) {
            raindrops[j++] = raindrops[i];
        }
    }
    rain_count = j;
}

void draw_rain() {
    for (int i = 0; i < rain_count; i++) {
        int ry = (int)raindrops[i].y;
        if (ry >= rows) continue;
        attr_t attr = COLOR_PAIR(COLOR_PAIR_RAIN_NORMAL);
        if (is_thunderstorm) attr |= A_BOLD;
        else if (raindrops[i].speed < 0.8) attr |= A_DIM;
        mvaddch(ry, raindrops[i].x % cols, raindrops[i].ch | attr);
    }
}

void start_bolt() {
    if (bolt_count >= MAX_BOLTS) return;
    LightningBolt b;
    b.max_y = rows;
    b.max_x = cols;
    b.seg_count = 1;
    b.segments[0].y = rand() % (rows / 5);
    b.segments[0].x = cols / 4 + rand() % (cols / 2);
    b.segments[0].created = get_time_sec();
    b.growing = 1;
    b.target_len = rows / 2 + rand() % (rows / 2);
    b.last_growth = get_time_sec();
    bolts[bolt_count++] = b;
}

void update_bolts() {
    double now = get_time_sec();
    int j = 0;
    for (int i = 0; i < bolt_count; i++) {
        LightningBolt *b = &bolts[i];
        if (b->growing && now - b->last_growth > 0.002) {
            b->last_growth = now;
            if (b->seg_count < b->target_len) {
                Segment last = b->segments[b->seg_count - 1];
                int offset = (rand() % 5) - 2;
                int nx = last.x + offset;
                if (nx < 0) nx = 0;
                if (nx >= cols) nx = cols - 1;
                int ny = last.y + 1;
                if (ny < rows) {
                    Segment s = {ny, nx, now};
                    b->segments[b->seg_count++] = s;
                } else {
                    b->growing = 0;
                }
            } else {
                b->growing = 0;
            }
        }
        int keep = 0;
        for (int k = 0; k < b->seg_count; k++) {
            if (now - b->segments[k].created <= 0.8) { keep = 1; break; }
        }
        if (keep) bolts[j++] = *b;
    }
    bolt_count = j;
}

void draw_bolts() {
    double now = get_time_sec();
    for (int i = 0; i < bolt_count; i++) {
        LightningBolt *b = &bolts[i];
        for (int k = 0; k < b->seg_count; k++) {
            double age = now - b->segments[k].created;
            if (age > 0.8) continue;
            char ch;
            if (age < 0.26) ch = '#';
            else if (age < 0.53) ch = '+';
            else ch = '*';
            mvaddch(b->segments[k].y, b->segments[k].x,
                    ch | COLOR_PAIR(COLOR_PAIR_LIGHTNING) | A_BOLD);
        }
    }
}

void init_cats() {
    if (!cats_enabled || rows < 6) { cat_count = 0; return; }

    if (cat_count_override > 0) {
        cat_count = cat_count_override;
        if (cat_count > MAX_CATS) cat_count = MAX_CATS;
    } else {
        cat_count = 3;
        if (cols < 35) cat_count = 1;
        else if (cols < 60) cat_count = 2;
    }

    int spacing = cols / (cat_count + 1);
    for (int i = 0; i < cat_count; i++) {
        int jitter = (spacing > 6) ? (rand() % (spacing / 3)) - spacing / 6 : 0;
        int base = spacing * (i + 1) - CAT_WIDTH / 2 + jitter;
        if (base < 0) base = 0;
        if (base + CAT_WIDTH >= cols) base = cols - CAT_WIDTH - 1;
        cats[i].x = (float)base;
        cats[i].state = CAT_SIT;
        cats[i].walk_timer = rand() % 150;
        cats[i].jumping = 0;
        cats[i].jump_frame = 0;
        cats[i].jump_step = 0;
        cats[i].cooldown = rand() % CAT_COOLDOWN; // stagger initial cooldowns
    }
}

void update_cats() {
    for (int i = 0; i < cat_count; i++) {
        Cat *c = &cats[i];

        if (c->cooldown > 0) c->cooldown--;

        if (c->jumping) {
            c->jump_step++;
            if (c->jump_step >= JUMP_STEP) {
                c->jump_step = 0;
                c->jump_frame++;
                if (c->jump_frame >= JUMP_FRAMES) {
                    c->jumping = 0;
                    c->jump_frame = 0;
                    c->x = c->jump_start_x + c->jump_dx; // land at arc end
                }
            }
            continue; // no walking mid-jump
        }

        c->walk_timer--;
        if (c->walk_timer <= 0) {
            int r = rand() % 10;
            if (r < 2)      c->state = CAT_WALK_L;
            else if (r < 4) c->state = CAT_WALK_R;
            else            c->state = CAT_SIT;
            c->walk_timer = 150 + rand() % 400; // long lazy pauses
        }

        if (c->state == CAT_WALK_L) {
            c->x -= 0.012f;
            if (c->x < 0) { c->x = 0; c->state = CAT_SIT; }
        } else if (c->state == CAT_WALK_R) {
            c->x += 0.012f;
            if (c->x + CAT_WIDTH >= cols) { c->x = cols - CAT_WIDTH - 1; c->state = CAT_SIT; }
        }
    }
}

void draw_cats() {
    attr_t cat_attr = COLOR_PAIR(COLOR_PAIR_CAT) | A_BOLD;

    for (int i = 0; i < cat_count; i++) {
        Cat *c = &cats[i];
        int cx = (int)c->x;
        if (cx < 0 || cx + CAT_WIDTH > cols) continue;

        int up = c->jumping ? jump_offsets[c->jump_frame] : 0;
        int feet_row = rows - 1 - up;
        if (feet_row - 1 < 0) continue; // clipped above screen top

        // Interpolate x smoothly along the arc
        if (c->jumping) {
            float t = (float)c->jump_frame / (JUMP_FRAMES - 1);
            cx = (int)(c->jump_start_x + t * c->jump_dx);
        }
        if (cx < 0 || cx + CAT_WIDTH > cols) continue;

        int airborne = (up > 0);

        attron(cat_attr);

        // Top row: ears + back
        mvaddstr(feet_row - 1, cx, " /\\_/\\ ");

        // Bottom row: face
        if (airborne) {
            mvaddstr(feet_row, cx, "(>^o^<)");
        } else if (c->state == CAT_WALK_L) {
            mvaddstr(feet_row, cx, "<(=^.^)");
        } else if (c->state == CAT_WALK_R) {
            mvaddstr(feet_row, cx, "(=^.^)>");
        } else {
            mvaddstr(feet_row, cx, "(=^.^=)");
        }

        attroff(cat_attr);
    }
}

int main(int argc, char *argv[]) {
    srand(time(NULL));
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);

    short rain_color = COLOR_CYAN;
    short lightning_color = COLOR_YELLOW;
    short cat_color = COLOR_WHITE;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--rain-color=", 13) == 0)
            rain_color = parse_color(argv[i] + 13);
        else if (strncmp(argv[i], "--lightning-color=", 18) == 0)
            lightning_color = parse_color(argv[i] + 18);
        else if (strcmp(argv[i], "--thunderstorm") == 0)
            is_thunderstorm = 1;
        else if (strcmp(argv[i], "--cats") == 0)
            cats_enabled = 1;
        else if (strncmp(argv[i], "--cat-color=", 12) == 0)
            cat_color = parse_color(argv[i] + 12);
        else if (strncmp(argv[i], "--cat-count=", 12) == 0)
            cat_count_override = atoi(argv[i] + 12);
    }

    init_colors(rain_color, lightning_color, cat_color);
    getmaxyx(stdscr, rows, cols);
    init_cats();

    while (1) {
        int ch = getch();
        if (ch == 'q' || ch == 'Q' || ch == 27) break;
        if (ch == 't' || ch == 'T') is_thunderstorm = !is_thunderstorm;
        if (ch == KEY_RESIZE) {
            getmaxyx(stdscr, rows, cols);
            clear();
            rain_count = 0;
            bolt_count = 0;
            init_cats();
        }

        float generation_chance = is_thunderstorm ? 0.5f : 0.3f;
        int max_new_drops = is_thunderstorm ? cols / 8 : cols / 15;

        if ((float)rand()/RAND_MAX < generation_chance) {
            int num_new = 1 + rand() % (max_new_drops > 0 ? max_new_drops : 1);
            for (int i = 0; i < num_new; i++) add_raindrop();
        }

        if (is_thunderstorm && bolt_count < 3 && ((float)rand()/RAND_MAX < 0.005f))
            start_bolt();

        update_rain();
        update_bolts();
        update_cats();

        clear();
        draw_bolts();
        draw_rain();
        draw_cats();
        refresh();

        usleep(UPDATE_INTERVAL);
    }

    endwin();
    return 0;
}
