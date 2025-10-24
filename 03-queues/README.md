# 03. Queues - Inter-Task Communication

## ภาพรวมหัวข้อ

หัวข้อนี้จะสอนการใช้ Queue สำหรับการสื่อสารระหว่าง Task

## 📋 เนื้อหาในหัวข้อ

### 📖 ทฤษฎี (1 ชั่วโมง)
- [FreeRTOS Queues - Inter-Task Communication](./03-queues.md) - เนื้อหาบรรยายหลัก
  - Queue Concepts และ FIFO behavior
  - Blocking vs Non-blocking Operations
  - Queue Sets และ Advanced Features
  - Producer-Consumer Patterns

### 💻 ปฏิบัติ (2 ชั่วโมง)
- [practice/](practice/) - โฟลเดอร์สำหรับการปฏิบัติ
  - [Lab 1: Basic Queue Operations](practice/lab1-basic-queue/) (45 นาที)
  - [Lab 2: Producer-Consumer System](practice/lab2-producer-consumer/) (45 นาที)
  - [Lab 3: Queue Sets Implementation](practice/lab3-queue-sets/) (30 นาที)

## 🎯 วัตถุประสงค์การเรียนรู้

เมื่อจบหัวข้อนี้ นักเรียนจะสามารถ:
1. ใช้ Queue สำหรับการสื่อสารระหว่าง Task
2. ออกแบบ Producer-Consumer systems
3. จัดการ blocking และ timeout operations
4. ใช้ Queue Sets สำหรับ multiple queues
5. Debug queue-related issues

## ⏱️ เวลาที่ใช้
- **ทฤษฎี**: 1 ชั่วโมง
- **ปฏิบัติ**: 2 ชั่วโมง
- **รวม**: 3 ชั่วโมง