#include "cron.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MIN_DELAY_US 1000 // 1ms，或根据需求调整

typedef struct cron_job_node {
    cron_job* job;
    struct cron_job_node* next;
} cron_node_t;

typedef struct {
    unsigned char running;
    TaskHandle_t handle;
    time_t seconds_until_next_execution;
    QueueHandle_t task_queue;
    esp_timer_handle_t esp_timer;
    cron_node_t* head;
    int next_id;
} cron_state_t;

static cron_state_t state = {
    .running = 0,
    .handle = NULL,
    .seconds_until_next_execution = -1,
    .task_queue = NULL,
    .esp_timer = NULL,
    .head = NULL,
    .next_id = 1,
};

// ====================== 内部工具 ========================

static void cron_job_list_insert(cron_job* job)
{
    cron_node_t* node = malloc(sizeof(cron_node_t));
    if (!node) {
        printf("Failed to allocate memory for cron node\n");
        return;
    }
    node->job = job;
    node->next = NULL;

    if (!state.head || job->next_execution < state.head->job->next_execution) {
        node->next = state.head;
        state.head = node;
        return;
    }

    cron_node_t* cur = state.head;
    while (cur->next && cur->next->job->next_execution <= job->next_execution) {
        cur = cur->next;
    }

    node->next = cur->next;
    cur->next = node;
}

static int cron_job_list_remove(int id)
{
    cron_node_t *cur = state.head, *prev = NULL;

    while (cur) {
        if (cur->job->id == id) {
            if (prev)
                prev->next = cur->next;
            else
                state.head = cur->next;
            free(cur);
            return 0;
        }
        prev = cur;
        cur = cur->next;
    }
    return -1;
}

static cron_job* cron_job_list_first()
{
    return state.head ? state.head->job : NULL;
}

static void schedule_next_timer();

// ====================== 定时器回调 ========================

static void timer_cb(void* arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    time_t now;
    time(&now);
    time_t now_sec = now;

    cron_job* due_jobs[16];
    int due_count = 0;
    while (1) {
        cron_job* job = cron_job_list_first();
        if (!job || job->next_execution > now)
            break;
        // 防抖：同一秒只触发一次
        if (job->last_triggered_sec == now_sec) {
            cron_job_list_remove(job->id);
            if (due_count < 16)
                due_jobs[due_count++] = job;
            continue;
        }
        job->last_triggered_sec = now_sec;
        xQueueSendFromISR(state.task_queue, &job, &xHigherPriorityTaskWoken);
        cron_job_list_remove(job->id);
        if (due_count < 16)
            due_jobs[due_count++] = job;
    }
    for (int i = 0; i < due_count; ++i) {
        cron_job_schedule(due_jobs[i]);
    }
    schedule_next_timer();
    if (xHigherPriorityTaskWoken)
        portYIELD_FROM_ISR();
}

static void schedule_next_timer()
{
    cron_job* job = cron_job_list_first();
    if (!job) {
        if (state.esp_timer)
            esp_timer_stop(state.esp_timer);
        return;
    }
    ESP_LOGI("cron", "Scheduling next job %d at %lld", job->id, job->next_execution);

    time_t now;
    time(&now);

    int64_t delay_us = (job->next_execution - now) * 1000000;
    if (delay_us < MIN_DELAY_US)
        delay_us = MIN_DELAY_US;

    esp_timer_stop(state.esp_timer);
    esp_timer_start_once(state.esp_timer, delay_us);

    state.seconds_until_next_execution = job->next_execution - now;
}

// ====================== worker task ========================

static void job_runner_task(void* arg)
{
    cron_job* job = (cron_job*)arg;

    TickType_t start_tick = xTaskGetTickCount();

    if (job && job->callback) {
        job->callback(job);
    }

    TickType_t elapsed = xTaskGetTickCount() - start_tick;
    if (elapsed > pdMS_TO_TICKS(5000)) { // 比如超过 5 秒
        printf("Warning: cron job %d callback took too long \n", job->id);
    }

    vTaskDelete(NULL);
}

static void cron_worker_task(void* arg)
{
    cron_job* job = NULL;
    while (1) {
        if (xQueueReceive(state.task_queue, &job, portMAX_DELAY)) {
            if (job) {
                // 在单独任务中执行 job.callback
                xTaskCreate(job_runner_task, "job_runner", 4096, job, tskIDLE_PRIORITY + 1, NULL);
            }
        }
    }
}

// ====================== API 实现 ========================

cron_job* cron_job_create(const char* schedule, cron_job_callback callback, void* data)
{
    cron_job* job = calloc(1, sizeof(cron_job));
    if (!job) {
        printf("Failed to allocate memory for cron job\n");
        return NULL;
    }

    job->callback = callback;
    job->data = data;
    job->id = state.next_id++;

    if (cron_job_load_expression(job, schedule) != 0) {
        free(job);
        return NULL;
    }

    if (cron_job_schedule(job) != 0) {
        free(job);
        return NULL;
    }

    return job;
}

int cron_job_destroy(cron_job* job)
{
    if (!job)
        return -1;
    cron_job_unschedule(job);
    free(job);
    return 0;
}

int cron_job_clear_all()
{
    while (state.head) {
        cron_job_destroy(state.head->job);
    }
    return 0;
}

int cron_stop()
{
    if (!state.running)
        return -1;

    state.running = 0;
    if (state.handle) {
        vTaskDelete(state.handle);
        state.handle = NULL;
    }

    if (state.esp_timer) {
        esp_timer_stop(state.esp_timer);
        esp_timer_delete(state.esp_timer);
        state.esp_timer = NULL;
    }

    if (state.task_queue) {
        vQueueDelete(state.task_queue);
        state.task_queue = NULL;
    }

    cron_job_clear_all();
    return 0;
}

int cron_start()
{
    if (state.running || state.handle)
        return -1;

    state.task_queue = xQueueCreate(10, sizeof(cron_job*));
    if (!state.task_queue) {
        printf("Failed to create task queue\n");
        return -1;
    }

    if (xTaskCreate(cron_worker_task, "cron_worker", 4096, NULL, tskIDLE_PRIORITY + 2, &state.handle) != pdPASS) {
        printf("Failed to create cron worker task\n");
        vQueueDelete(state.task_queue);
        return -1;
    }

    esp_timer_create_args_t timer_args = {
        .callback = timer_cb,
        .name = "cron_timer"
    };

    if (esp_timer_create(&timer_args, &state.esp_timer) != ESP_OK) {
        vTaskDelete(state.handle);
        vQueueDelete(state.task_queue);
        return -1;
    }

    state.running = 1;
    schedule_next_timer();
    return 0;
}

int cron_job_schedule(cron_job* job)
{
    if (!job || !cron_job_has_loaded(job))
        return -1;

    time_t now;
    time(&now);

    job->next_execution = cron_next(&(job->expression), now);
    job->last_triggered_sec = -1; // 每次重新调度时重置
    cron_job_list_insert(job);

    // 不再自动 schedule_next_timer，统一由 timer_cb 或外部调用
    return 0;
}

int cron_job_unschedule(cron_job* job)
{
    if (!job)
        return -1;
    return cron_job_list_remove(job->id);
}

int cron_job_load_expression(cron_job* job, const char* schedule)
{
    if (!job || !schedule)
        return -1;

    memset(&(job->expression), 0, sizeof(job->expression));
    const char* error = NULL;
    cron_parse_expr(schedule, &(job->expression), &error);

    if (error) {
        printf("Failed to parse cron expression: %s\n", error);
        return -1;
    }

    job->load = &(job->expression);
    return 0;
}

int cron_job_has_loaded(cron_job* job)
{
    return job && (job->load == &(job->expression));
}

time_t cron_job_seconds_until_next_execution()
{
    return state.seconds_until_next_execution;
}
