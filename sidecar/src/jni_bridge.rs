use jni::objects::{JClass, JString};
use jni::sys::{jint, jstring};
use jni::JNIEnv;
use std::ffi::CString;

#[no_mangle]
pub extern "system" fn Java_org_sonicr_android_NetplayBridge_nativeInit(
    _env: JNIEnv,
    _class: JClass,
) -> jint {
    crate::ffi::netplay_init() as jint
}

#[no_mangle]
pub extern "system" fn Java_org_sonicr_android_NetplayBridge_nativeStartHost(
    mut env: JNIEnv,
    _class: JClass,
    j_hub_url: JString,
    j_room_name: JString,
    game_port: jint,
) -> jint {
    let hub_url = match env.get_string(&j_hub_url) {
        Ok(s) => CString::new(s.to_str().unwrap_or("")).unwrap_or_default(),
        Err(_) => return -1,
    };

    let room_name = if !j_room_name.is_null() {
        match env.get_string(&j_room_name) {
            Ok(s) => CString::new(s.to_str().unwrap_or("")).ok(),
            Err(_) => None,
        }
    } else {
        None
    };

    let r_name_ptr = room_name.as_ref().map(|s| s.as_ptr()).unwrap_or(std::ptr::null());

    crate::ffi::netplay_start_host(hub_url.as_ptr(), r_name_ptr, game_port as u16) as jint
}

#[no_mangle]
pub extern "system" fn Java_org_sonicr_android_NetplayBridge_nativeStartJoin(
    mut env: JNIEnv,
    _class: JClass,
    j_hub_url: JString,
    j_room_id: JString,
    game_port: jint,
) -> jint {
    let hub_url = match env.get_string(&j_hub_url) {
        Ok(s) => CString::new(s.to_str().unwrap_or("")).unwrap_or_default(),
        Err(_) => return -1,
    };

    let room_id = if !j_room_id.is_null() {
        match env.get_string(&j_room_id) {
            Ok(s) => CString::new(s.to_str().unwrap_or("")).ok(),
            Err(_) => None,
        }
    } else {
        None
    };

    let r_id_ptr = room_id.as_ref().map(|s| s.as_ptr()).unwrap_or(std::ptr::null());

    crate::ffi::netplay_start_join(hub_url.as_ptr(), r_id_ptr, game_port as u16) as jint
}

#[no_mangle]
pub extern "system" fn Java_org_sonicr_android_NetplayBridge_nativeStop(
    _env: JNIEnv,
    _class: JClass,
) {
    crate::ffi::netplay_stop();
}

#[no_mangle]
pub extern "system" fn Java_org_sonicr_android_NetplayBridge_nativeFetchRoomList(
    mut env: JNIEnv,
    _class: JClass,
    j_hub_url: JString,
) -> jstring {
    let hub_url = match env.get_string(&j_hub_url) {
        Ok(s) => s.to_str().unwrap_or("").to_string(),
        Err(_) => return std::ptr::null_mut(),
    };

    match crate::fetch_room_list(&hub_url) {
        Ok(json_str) => match env.new_string(json_str) {
            Ok(js) => js.into_raw(),
            Err(e) => {
                tracing::error!("Failed to allocate JNI string: {e}");
                std::ptr::null_mut()
            }
        },
        Err(e) => {
            tracing::error!("Failed to fetch room list: {e}");
            std::ptr::null_mut()
        }
    }
}

#[no_mangle]
pub extern "system" fn Java_org_sonicr_android_NetplayBridge_fetchRoomList(
    env: JNIEnv,
    class: JClass,
    j_hub_url: JString,
) -> jstring {
    Java_org_sonicr_android_NetplayBridge_nativeFetchRoomList(env, class, j_hub_url)
}


