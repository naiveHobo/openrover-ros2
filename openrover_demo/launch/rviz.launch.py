from pathlib import Path

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import SetEnvironmentVariable
from launch_ros.actions import Node


def generate_launch_description():
    rviz_config = Path(get_package_share_directory('openrover_demo'), 'config', 'default.rviz')
    assert rviz_config.is_file()

    return LaunchDescription([
        SetEnvironmentVariable('RCUTILS_CONSOLE_STDOUT_LINE_BUFFERED', '1'),
        Node(
            package='rviz2', node_executable='rviz2', output='screen',
            arguments=['--display-config', str(rviz_config)]
        )
    ])