cmake_minimum_required(VERSION 3.22)
project(eurobot_interfaces)
find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)
find_package(std_msgs REQUIRED)
find_package(service_msgs REQUIRED)
find_package(action_msgs REQUIRED)
rosidl_generate_interfaces(${PROJECT_NAME}
  "action/Gripper.action" "msg/ControllerState.msg" "msg/BoardInputs.msg" "srv/Arm.srv" "srv/Maintenance.srv" "srv/SetOutputs.srv" "srv/SetActuator.srv"
  DEPENDENCIES std_msgs service_msgs action_msgs)
ament_export_dependencies(rosidl_default_runtime)
ament_package()
