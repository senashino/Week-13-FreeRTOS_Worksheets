# 04. Semaphores and Mutexes

## ภาพรวมหัวข้อ

หัวข้อนี้จะสอนการใช้ Semaphores และ Mutexes ใน FreeRTOS สำหรับการ synchronization และป้องกัน race conditions

## 📋 เนื้อหาในหัวข้อ

### 📖 ทฤษฎี (1 ชั่วโมง)
- [Semaphores and Mutexes](./04-semaphores.md) - เนื้อหาบรรยายหลัก
  - Binary Semaphores สำหรับ Synchronization
  - Counting Semaphores สำหรับการจัดการทรัพยากร
  - Mutexes และ Critical Sections
  - Priority Inheritance และการป้องกัน Priority Inversion

### 💻 ปฏิบัติ (2 ชั่วโมง)
- [practice/](practice/) - โฟลเดอร์สำหรับการปฏิบัติ
  - [Lab 1: Binary Semaphores](practice/lab1-binary-semaphores/) (45 นาที)
  - [Lab 2: Mutex and Critical Sections](practice/lab2-mutex-critical-sections/) (45 นาที)
  - [Lab 3: Counting Semaphores](practice/lab3-counting-semaphores/) (30 นาที)

## 🎯 วัตถุประสงค์การเรียนรู้

เมื่อจบหัวข้อนี้ นักเรียนจะสามารถ:
1. เข้าใจหลักการของ Semaphores and Mutexes
2. ใช้งาน APIs ที่เกี่ยวข้อง
3. ออกแบบระบบที่ใช้ Semaphores and Mutexes
4. Debug และ troubleshoot ปัญหา
5. ประยุกต์ใช้ในโปรเจกต์จริง

## ⏱️ เวลาที่ใช้
- **ทฤษฎี**: 1 ชั่วโมง
- **ปฏิบัติ**: 2 ชั่วโมง
- **รวม**: 3 ชั่วโมง
