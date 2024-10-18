// Copyright 2024, Linaro Limited
// Author(s): Manos Pitsidianakis <manos.pitsidianakis@linaro.org>
// SPDX-License-Identifier: GPL-2.0-or-later

#[macro_export]
macro_rules! device_class_init {
    ($func:ident, props => $props:ident, realize_fn => $realize_fn:expr, legacy_reset_fn => $legacy_reset_fn:expr, vmsd => $vmsd:ident$(,)*) => {
        pub unsafe extern "C" fn $func(
            klass: *mut $crate::bindings::ObjectClass,
            _: *mut ::core::ffi::c_void,
        ) {
            let mut dc =
                ::core::ptr::NonNull::new(klass.cast::<$crate::bindings::DeviceClass>()).unwrap();
            unsafe {
                dc.as_mut().realize = $realize_fn;
                dc.as_mut().vmsd = &$vmsd;
                $crate::bindings::device_class_set_legacy_reset(dc.as_mut(), $legacy_reset_fn);
                $crate::bindings::device_class_set_props(dc.as_mut(), $props.as_ptr());
            }
        }
    };
}

#[macro_export]
macro_rules! define_property {
    ($name:expr, $state:ty, $field:expr, $prop:expr, $type:expr, default = $defval:expr$(,)*) => {
        $crate::bindings::Property {
            // use associated function syntax for type checking
            name: ::core::ffi::CStr::as_ptr($name),
            info: $prop,
            offset: ::core::mem::offset_of!($state, $field) as isize,
            set_default: true,
            defval: $crate::bindings::Property__bindgen_ty_1 { u: $defval as u64 },
            ..unsafe { ::core::mem::MaybeUninit::<$crate::bindings::Property>::zeroed().assume_init() }
        }
    };
    ($name:expr, $state:ty, $field:expr, $prop:expr, $type:expr$(,)*) => {
        $crate::bindings::Property {
            // use associated function syntax for type checking
            name: ::core::ffi::CStr::as_ptr($name),
            info: $prop,
            offset: ::core::mem::offset_of!($state, $field) as isize,
            set_default: false,
            ..unsafe { ::core::mem::MaybeUninit::<$crate::bindings::Property>::zeroed().assume_init() }
        }
    };
}

#[macro_export]
macro_rules! declare_properties {
    ($ident:ident, $($prop:expr),*$(,)*) => {
        pub static $ident: [$crate::bindings::Property; {
            let mut len = 1;
            $({
                _ = stringify!($prop);
                len += 1;
            })*
            len
        }] = [
            $($prop),*,
            unsafe { ::core::mem::MaybeUninit::<$crate::bindings::Property>::zeroed().assume_init() },
        ];
    };
}

#[macro_export]
macro_rules! vm_state_description {
    ($(#[$outer:meta])*
     $name:ident,
     $(name: $vname:expr,)*
     $(unmigratable: $um_val:expr,)*
    ) => {
        #[used]
        $(#[$outer])*
        pub static $name: $crate::bindings::VMStateDescription = $crate::bindings::VMStateDescription {
            $(name: {
                #[used]
                static VMSTATE_NAME: &::core::ffi::CStr = $vname;
                $vname.as_ptr()
            },)*
            unmigratable: true,
            ..unsafe { ::core::mem::MaybeUninit::<$crate::bindings::VMStateDescription>::zeroed().assume_init() }
        };
    }
}
