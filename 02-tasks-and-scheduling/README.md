# 02. Tasks and Scheduling

## ภาพรวมหัวข้อ

หัวข้อนี้จะสอนการจัดการ Task และการทำงานของ Scheduler ใน FreeRTOS

## 📋 เนื้อหาในหัวข้อ

### 📖 ทฤษฎี (1 ชั่วโมง)
- [FreeRTOS Tasks and Scheduling](./02-tasks-and-scheduling.md) - เนื้อหาบรรยายหลัก
  - Task States และ State Transitions
  - Priority-based Preemptive Scheduling
  - Stack Management และ Memory Allocation
  - Task Control Block (TCB)

### 💻 ปฏิบัติ (2 ชั่วโมง)
- [practice/](practice/) - โฟลเดอร์สำหรับการปฏิบัติ
  - [Lab 1: Task Priority และ Scheduling](practice/lab1-task-priority/) (45 นาที)
  - [Lab 2: Task States Demonstration](practice/lab2-task-states/) (45 นาที)
  - [Lab 3: Stack Monitoring และ Debugging](practice/lab3-stack-monitoring/) (30 นาที)

## 🎯 วัตถุประสงค์การเรียนรู้

เมื่อจบหัวข้อนี้ นักเรียนจะสามารถ:
1. อธิบาย Task States และการเปลี่ยนแปลง
2. จัดการ Task Priority และเข้าใจ Scheduling behavior
3. Monitor และ Debug Task performance
4. จัดการ Stack memory อย่างมีประสิทธิภาพ
5. ใช้ Task management APIs อย่างเหมาะสม

## ⏱️ เวลาที่ใช้
- **ทฤษฎี**: 1 ชั่วโมง
- **ปฏิบัติ**: 2 ชั่วโมง
- **รวม**: 3 ชั่วโมง