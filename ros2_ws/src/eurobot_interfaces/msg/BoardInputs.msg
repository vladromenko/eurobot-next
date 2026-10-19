# Raw electrical GPIO levels; a bit is meaningful only when present in known_mask.
# A configured pin's active-high/low semantics belong to the deployment profile.
std_msgs/Header header
uint16 levels
uint16 known_mask
bool battery_sample_known
uint8 query_error
uint8 battery_error
